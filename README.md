# broadcast_send：UDPブロードキャスト負荷送信ツール

既存受信プログラムに負荷を与えるため、同じ1個のUDPデータグラムを、サブネットのブロードキャストアドレス宛てに周期送信するツール。対象はRHEL 8。仕様と設計の背景は [PLAN.md](PLAN.md) を参照。

| ファイル | 内容 |
|---|---|
| `broadcast_send` | 送信プログラム（C） |
| `scripts/monitor_net.sh` | OS・NIC統計の観測スクリプト。送信側・受信側の両方で使う |
| `udp_recv_check` | 検証用受信ツール。受信データがペイロードとバイト単位で一致するかを確認する |
| `examples/` | コンフィグ・ペイロードのサンプル |

## ビルド

必要なパッケージ：`gcc`、`make`。観測には`iproute`、`ethtool`、`sysstat`を使う。

```sh
make          # broadcast_send と udp_recv_check を作成
make test     # 単体テストとCLIテスト
```

## 使い方

```sh
./broadcast_send --config examples/send.ini --check-config   # 静的検証だけ。送信しない
./broadcast_send --config examples/send.ini                  # 送信
./broadcast_send --config examples/send.ini --stats-file run01.csv   # 統計CSVの出力先を変える
```

- `--check-config`はコンフィグとペイロードを検証し、予定負荷を表示する。NIC・アドレスの検証は通常起動時に行う。
- 終了コードは、0が正常終了、1が入力エラー（コンフィグ・ペイロード・NIC・アドレス・統計ファイル）、2が実行時エラー。
- 入力エラーがあれば送信を開始しない。
- 終了条件は、`duration_sec`、`max_attempts`、SIGINT・SIGTERMのうち先に到達したもの。バーストの途中でも確認する。
- 終了時は、どの終了条件でも最終統計を出力する。

一般ユーザーで実行できる（特権は不要）。1024未満の`source_port`を指定する場合は、権限が必要になる。

### コンフィグ

`examples/send.ini`が完全な例。

| セクション | キー | 必須 | 内容 |
|---|---|---|---|
| network | `interface` | ○ | 送信インターフェース。存在・UP・ブロードキャスト対応を起動時に確認 |
| network | `source_ip` | ○ | そのインターフェースに設定されたIPv4アドレス |
| network | `source_port` | ○ | 0はOSによる自動割当。1〜65535で固定 |
| network | `broadcast_ip` | ○ | `source_ip`とネットマスクから求まるブロードキャストアドレス。一致しなければエラー |
| network | `destination_port` | ○ | 1〜65535 |
| payload | `format` | ○ | `binary`または`hex` |
| payload | `file` | ○ | ファイル全体が1個のUDPデータ部になる |
| send | `period_us` | ○ | 周期（μs）。100〜3,600,000,000 |
| send | `packets_per_cycle` | ○ | 1周期に連続送信する個数。1〜1,000,000 |
| send | `duration_sec` | | 0（既定）は時間制限なし |
| send | `max_attempts` | | 0（既定）は試行数制限なし。成功と失敗を合わせた送信試行数 |
| stats | `interval_sec` | | 区間集計の間隔。既定は1。0は終了時の集計だけ |
| stats | `file` | | 統計CSV。既存ファイルは上書きしない。省略時はCSVを作らず、区間集計を標準出力に出す |

構文：

- 空行と、行頭が`#`・`;`の行は無視する。行末のコメントは書けない。
- 値の前後の空白は除去する。CRLF改行も受け付ける。
- 次の場合は、ファイル名と行番号付きのエラーにする。
  - 未定義のセクション・キー
  - 重複キー
  - 空の値
  - 範囲外や不正な数値（符号・指数・16進表記は不可）
- 必須項目が欠けている場合もエラーにする。
- 相対パス（`payload.file`、`stats.file`）は、コンフィグファイルのあるディレクトリを基準に解決する。`--stats-file`はカレントディレクトリを基準にする。

### ペイロード

- データ部は1〜65,507バイト。65,507バイトはIPv4オプションなしの上限。
- 切り詰め・ゼロ埋め・独自ヘッダー・終端NULの追加は行わない。
- データは起動時に1回だけ読み込む。
- `binary`は、ファイル内容をそのまま送信する。NULを含んでもよい。
- `hex`は、`00 01 FF`のような2桁の16進数を並べる。
  - 区切りは空白・タブ・改行（CRLF可）。区切らずに`0001FF`と書いてもよい。
  - 大文字・小文字は区別しない。
  - `0x`表記、カンマ、コメント、奇数桁は、行・桁の位置付きでエラーにする。

## 送信の動作

- 送信先はIPv4 UDPで、`SO_BROADCAST`を有効にして送る。
- 送信元は`bind()`で指定し、`sendmsg()`の`IP_PKTINFO`でインターフェースと送信元アドレスを明示する。
- `IP_MTU_DISCOVER = IP_PMTUDISC_DONT`でIP分割を許可する。システム全体の設定は変更しない。
- 周期は、`CLOCK_MONOTONIC`の絶対時刻で待機する（`clock_nanosleep`）。周期kの予定時刻は「開始時刻＋k×周期」。
  - 起床の遅れが1周期未満なら、その周期を実行し、遅れを記録する。
  - 起床の遅れが1周期以上なら、過ぎた周期をスキップし、現在の周期を1回だけ実行する。
  - バースト（1周期分の送信）が次の予定時刻を過ぎたら、その周期をスキップする。取り戻すための連続送信はしない。
  - 終了時刻以降の予定周期と、終了によるバーストの残りは、スキップに数えない。
- 送信は非ブロッキングで行う。
  - `EAGAIN`、`ENOBUFS`は、失敗として数えて次の試行へ進む。再送はしない。
  - `EINTR`は、同じ試行を再開する。
  - その他のエラーは継続できないものとして扱い、最終統計を出して終了する（終了コード2）。

### 送信統計

起動時に、設定内容、予定pps、データ部だけの予定負荷、MTU、IP分割の有無、`SO_SNDBUF`を表示する。

CSVの列：

| 列 | 内容 |
|---|---|
| `record` | `interval`（区間）または`total`（全体） |
| `utc_time`、`elapsed_s`、`interval_s` | 記録時刻（UTC）、開始からの経過秒、集計対象の秒数 |
| `cycles`、`skipped_cycles`、`skipped_packets` | 実行周期数、スキップ周期数、未送信相当（スキップ周期数×個数/周期） |
| `attempts`、`ok`、`fail` | 送信試行数、送信API成功数、送信API失敗数 |
| `fail_eagain`、`fail_enobufs`、`fail_other` | errno別の失敗数 |
| `ok_payload_bytes`、`ok_pps`、`ok_payload_bytes_per_s` | API成功分のデータ部バイト数と、実経過時間で割ったレート |
| `late_avg_us`、`late_max_us` | 周期開始の予定時刻からの遅れ |
| `cycle_gap_min_us`、`cycle_gap_max_us` | 実際の周期開始間隔 |
| `burst_max_us` | 1周期の送信処理時間の最大 |

`ok`は、OSの送信APIが受け付けた数である。NICから実際に送信された数ではなく、受信側への到達も意味しない。スキップ数も、OSやNICでのDROP数ではない。

## 観測スクリプト

```sh
scripts/monitor_net.sh -i ens192 -p 50000                  # Ctrl+Cまで1秒間隔で採取
scripts/monitor_net.sh -i ens192 -p 50000 -d 120 -o run01_rx   # 120秒後に自動で終了
```

オプション：

| オプション | 内容 |
|---|---|
| `-i` | 対象インターフェース（必須） |
| `-o` | 出力ディレクトリ。既定は`monitor_<host>_<iface>_<UTC>`。空でないディレクトリには書かない |
| `-t` | 採取間隔（秒）。既定は1 |
| `-d` | 観測時間（秒）。既定は0（無制限） |
| `-p` | `ss`で絞り込むUDPポート |

出力ファイル：

| ファイル | 内容 |
|---|---|
| `meta.txt` | 実行環境。OS、カーネル、`ip -d link`、`ethtool`（速度、ドライバー、オフロード、リング、チャネル、割込み結合）、関連するsysctl（ソケットバッファ、`netdev_max_backlog`、IP再構成）、パッケージ版 |
| `iface.csv` | インターフェースのRX/TXのバイト・パケット・DROP・エラー。累積値と差分 |
| `udpip.csv` | nstatによるUDP/IP統計。累積値と差分 |
| `ethtool.csv` | NIC固有統計（`ethtool -S`の全項目）。1行1項目の形式 |
| `raw/` | `ip -s -s link`、`ethtool -S`、`ss -u -a -n -m`、`/proc/net/softnet_stat`、`sar -n DEV,EDEV`、`mpstat -P ALL`の元出力 |
| `events.log` | 開始、終了、取得不可のツール、カウンターのリセット検出、採取の遅れ |

`udpip.csv`の主な項目：

- UDP：`UdpInDatagrams`、`UdpOutDatagrams`、`UdpInErrors`、`UdpRcvbufErrors`、`UdpSndbufErrors`、`UdpNoPorts`
- IP分割・再構成：`IpFragOKs`、`IpFragCreates`、`IpFragFails`、`IpReasmReqds`、`IpReasmOKs`、`IpReasmFails`

差分列の値：

- 数値は、前回の採取からの増分。
- `NA`は、初回の採取、またはツール・カウンターがなく取得できないことを示す。0とは区別する。
- `RESET`は、累積値が減少したこと（カウンターのリセット等）を示す。負の値は出さない。

注意点：

- nstatは`-asz`で実行するため、nstatの履歴ファイルは更新しない。
- `IpReasmTimeout`は件数ではなく設定値なので、差分を集計しない。
- 各層のDROPは重複して数えられる場合があるため、単純に合算しない。
- IP分割を使う場合、NICのフレーム数とUDPデータグラム数は一致しない。
- 終了はCtrl+C（SIGINT）またはSIGTERMで、最後に1回採取してから終了する。
- スクリプトから`&`でバックグラウンド起動すると、bashの仕様でSIGINTが無視される。その場合はSIGTERM（`kill PID`）で止める。

## 実機での測定手順（RHEL 8、2台構成）

1. 両ホストで環境を確認する。
   - `ethtool <NIC>`でリンク速度（1000Mb/s）を確認する。
   - `ip link`でMTUを確認する。
   - 受信側は`sysctl net.ipv4.ipfrag_time`（IP再構成の待機時間）も確認する。
2. 送信側でコンフィグを作り、`--check-config`で予定負荷を確認する。
3. 観測を開始する。両ホストで`scripts/monitor_net.sh -i <NIC> -p <port> -o <名前>`を実行し、数秒間の基準値を取る。
4. 受信側で既存受信プログラムを起動する。
5. 送信側で`broadcast_send`を実行する。標準出力もファイルに残す（`| tee run01.log`）。
6. 送信の終了後も観測を続ける。IP分割の試験では、`ipfrag_time`（既定30秒）以上続ける。受信できなかった断片の再構成失敗は、遅れて計上されるため。その後、観測をCtrl+Cで止める。
7. 周期、データサイズ、個数/周期を段階的に変えて繰り返す。

負荷の目安（UDPデータ部のみの計算値。ヘッダー、IP分割、フレーム間隔を含まない）：

| データ部 | 周期 | 個数/周期 | 予定pps | データ部の負荷 |
|---|---|---|---|---|
| 100 B | 100 μs | 1 | 10,000 | 8 Mbps |
| 1472 B | 100 μs | 1 | 10,000 | 117.76 Mbps |
| 1472 B | 100 μs | 8 | 80,000 | 942.08 Mbps |
| 65,507 B | 1000 μs | 1 | 1,000 | 524.056 Mbps |
| 65,507 B | 100 μs | 1 | 10,000 | 5.24 Gbps（過負荷） |

### 送信内容の確認（任意）

受信側で既存受信プログラムの代わりに`udp_recv_check`を起動すると、次を確認できる。

- 受信データとペイロードファイルのバイト一致
- 最初のデータグラムの送信元アドレス・ポート、宛先アドレス、受信インターフェース

```sh
./udp_recv_check --port 50000 --payload examples/payload.hex --format hex --idle-timeout 5
```

## 検証状況

開発環境（WSL2上のLinux、仮想NIC eth0、MTU 1500）で確認した内容：

- 単体テスト（`make test`）。ASan・UBSan付きでも実行した。
  - スケジューラー：PLAN.mdの例（350μsの起床で2周期スキップ、450μsのバースト終了で1周期スキップ）、軽微な遅れ、終了時刻の境界、1年分の周期番号
  - 設定：正常系、各エラー、行番号、CRLF、NUL、相対パス
  - ペイロード：binaryとhexの一致、NULを含むデータ、65,507バイトの受理と65,508バイトの拒否（両形式）、hexの不正文字・奇数桁
  - CLI：静的検証、入力エラー時に送信を開始しないこと、存在しないNIC
- 実送信と受信照合（同一ホスト）。
  - 3個/周期で300個送信し、送信元・ブロードキャスト宛先・ポート・内容が300個とも一致した。
  - 65,507バイト（NULを含むバイナリ）でも300個とも一致した。
- IP分割（nstatで確認）。
  - 1,472バイトは分割なし。
  - 1,473バイトは1データグラムあたり2断片、65,507バイトは45断片。
- 過負荷時（65,507バイト×8個/周期、100μs）。
  - `EAGAIN`を失敗として記録し、送信を継続した。
  - 実行周期数とスキップ周期数の合計が、予定の周期数（1秒で10,000）と一致した。
- 終了処理：SIGINT・SIGTERM・試行数上限で、バーストの途中でも終了し、最終統計を出力した。
- 観測スクリプト：採取、差分、`NA`・`RESET`、SIGINT・SIGTERMでの終了。

開発環境では確認できず、RHEL 8の実機で確認が必要な項目：

- RHEL 8（gcc 8）でのビルド
- 1Gbpsの物理NICでの100μs周期の実測遅れ、受付pps、NIC通信量
- 別ホストの受信側での、IP再構成とDROPの観測。同一ホスト内では、分割前のデータグラムがそのまま配送されるため再構成は発生しない
- `sar`・`mpstat`の出力。開発環境にsysstatがないため未確認
- 実NICの`ethtool -S`の項目
