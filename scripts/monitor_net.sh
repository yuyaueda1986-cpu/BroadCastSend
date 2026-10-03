#!/bin/bash
# OS・NICの統計を一定間隔で採取する（送信側・受信側の両方で同じ手順で使用する）。
#
#   scripts/monitor_net.sh -i IFACE [-o OUTDIR] [-t INTERVAL_SEC] [-d DURATION_SEC] [-p UDP_PORT]
#
# 出力（OUTDIR）:
#   meta.txt        実行環境（OS・カーネル・NIC・ドライバー・MTU・オフロード・リング・バッファ設定など）
#   iface.csv       /sys/class/net/IFACE/statistics の累積値と差分（ip -s -s link と同じカウンター）
#   udpip.csv       nstat -asz によるUDP/IP統計の累積値と差分
#   ethtool.csv     ethtool -S のNIC固有統計（縦持ち：name,value,delta）
#   raw/*.log       ip -s -s link / ethtool -S / ss -u -a -n -m / softnet_stat / sar / mpstat の元出力
#   events.log      開始・終了・カウンターのリセット検出などの記録
#
# 差分列の値:
#   数値   前回採取からの増分
#   NA     取得不可（ツールやカウンターがない、または初回採取）
#   RESET  累積値が減少した（カウンターのリセット等）。負の値は出さない
#
# SIGINT / SIGTERM または -d の時間経過で、最後の採取をしてから終了する。

set -u
export LC_ALL=C
export S_TIME_FORMAT=ISO

usage() {
    # ASCII terminal messages work with UTF-8, EUC-JP and SJIS terminals.
    cat <<'HELP'
Usage: monitor_net.sh -i IFACE [-o OUTDIR] [-t SECONDS] [-d SECONDS] [-p PORT]
  -i IFACE    Network interface to monitor (required)
  -o OUTDIR   Output directory (must be empty or new)
  -t SECONDS  Sample interval (default: 1)
  -d SECONDS  Duration (default: 0 = until Ctrl+C)
  -p PORT     UDP port filter (1-65535)
  -h          Show this help
Files: meta.txt, iface.csv, udpip.csv, ethtool.csv, raw/, events.log
Saved text uses UTF-8; CSV numeric fields use the C locale.
HELP
    exit "${1:-1}"
}

IFACE=""
OUTDIR=""
INTERVAL=1
DURATION=0
PORT=""

while getopts "i:o:t:d:p:h" opt; do
    case "$opt" in
    i) IFACE=$OPTARG ;;
    o) OUTDIR=$OPTARG ;;
    t) INTERVAL=$OPTARG ;;
    d) DURATION=$OPTARG ;;
    p) PORT=$OPTARG ;;
    h) usage 0 ;;
    *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || usage 1
[ -n "$IFACE" ] || { echo "Error: specify -i IFACE" >&2; usage 1; }
[[ "$INTERVAL" =~ ^[1-9][0-9]*$ ]] || { echo "Error: -t must be an integer >= 1 (seconds)" >&2; exit 1; }
[[ "$DURATION" =~ ^[0-9]+$ ]] || { echo "Error: -d must be an integer >= 0 (seconds)" >&2; exit 1; }
if [ -n "$PORT" ] && ! [[ "$PORT" =~ ^[0-9]+$ && "$PORT" -ge 1 && "$PORT" -le 65535 ]]; then
    echo "Error: -p must be 1-65535" >&2; exit 1
fi
[ -d "/sys/class/net/$IFACE" ] || { echo "Error: interface '$IFACE' does not exist" >&2; exit 1; }

HOST=$(hostname)
if [ -z "$OUTDIR" ]; then
    OUTDIR="monitor_${HOST}_${IFACE}_$(date -u +%Y%m%dT%H%M%SZ)"
fi
if [ -e "$OUTDIR" ] && [ -n "$(ls -A "$OUTDIR" 2>/dev/null)" ]; then
    echo "Error: output directory '$OUTDIR' is not empty; refusing to overwrite" >&2
    exit 1
fi
mkdir -p "$OUTDIR/raw" || exit 1

have() { command -v "$1" >/dev/null 2>&1; }
utc_now() { local t; t=$(date -u +%Y-%m-%dT%H:%M:%S.%N); echo "${t:0:26}Z"; }
uptime_now() { cut -d' ' -f1 /proc/uptime; }
log_event() { echo "$(utc_now) $*" >> "$OUTDIR/events.log"; }

# ---------------------------------------------------------------- 実行環境の記録
snap() { # snap <title> <command...>
    {
        echo "===== $1"
        echo "\$ ${*:2}"
        if have "$2"; then
            "${@:2}" 2>&1
            echo "(exit $?)"
        else
            echo "(取得不可: $2 がありません)"
        fi
        echo
    } >> "$OUTDIR/meta.txt"
}

{
    echo "host: $HOST"
    echo "interface: $IFACE"
    echo "start_utc: $(utc_now)"
    echo "uptime_s: $(uptime_now)"
    echo "interval_sec: $INTERVAL"
    echo "duration_sec: $DURATION"
    echo "udp_port: ${PORT:-(指定なし)}"
    echo "os_release: $(cat /etc/redhat-release 2>/dev/null || grep PRETTY_NAME /etc/os-release 2>/dev/null)"
    echo
} > "$OUTDIR/meta.txt"
snap "kernel" uname -a
snap "link" ip -d link show dev "$IFACE"
snap "address" ip addr show dev "$IFACE"
snap "link speed/duplex" ethtool "$IFACE"
snap "driver" ethtool -i "$IFACE"
snap "offload" ethtool -k "$IFACE"
snap "ring" ethtool -g "$IFACE"
snap "channels" ethtool -l "$IFACE"
snap "coalesce" ethtool -c "$IFACE"
snap "sysctl" sysctl net.core.rmem_default net.core.rmem_max net.core.wmem_default net.core.wmem_max \
    net.core.netdev_max_backlog net.core.netdev_budget net.ipv4.udp_mem net.ipv4.udp_rmem_min \
    net.ipv4.udp_wmem_min net.ipv4.ipfrag_high_thresh net.ipv4.ipfrag_low_thresh net.ipv4.ipfrag_time
snap "packages" rpm -q iproute ethtool sysstat
snap "cpu" nproc

# ---------------------------------------------------------------- カウンター定義
IFACE_KEYS=(rx_bytes rx_packets rx_errors rx_dropped rx_missed_errors rx_fifo_errors rx_over_errors
    rx_crc_errors rx_frame_errors rx_length_errors multicast
    tx_bytes tx_packets tx_errors tx_dropped tx_fifo_errors tx_carrier_errors collisions)

# IpReasmTimeoutは設定値（秒）であり件数ではないため採取しない
NSTAT_KEYS=(UdpInDatagrams UdpOutDatagrams UdpInErrors UdpRcvbufErrors UdpSndbufErrors UdpNoPorts
    UdpInCsumErrors IpInReceives IpInDiscards IpInDelivers IpOutRequests IpOutDiscards
    IpFragOKs IpFragFails IpFragCreates IpReasmReqds IpReasmOKs IpReasmFails
    IpExtInBcastPkts IpExtOutBcastPkts)

declare -A PREV       # 前回値（キー: 種別/名前）
declare -A CUR

# delta <key> : CUR[key]とPREV[key]から差分文字列を返す
delta() {
    local k=$1 c=${CUR[$1]:-NA} p=${PREV[$1]:-NA}
    if [ "$c" = NA ] || [ "$p" = NA ]; then
        echo NA
    elif [ "$c" -lt "$p" ]; then
        log_event "RESET $k: $p -> $c"
        echo RESET
    else
        echo $((c - p))
    fi
}

csv_header() { # csv_header <file> <keys...>
    local f=$1 h="utc_time,uptime_s,interval_s" k
    shift
    for k in "$@"; do h+=",$k,${k}_delta"; done
    echo "$h" > "$f"
}

csv_header "$OUTDIR/iface.csv" "${IFACE_KEYS[@]}"
csv_header "$OUTDIR/udpip.csv" "${NSTAT_KEYS[@]}"
echo "utc_time,uptime_s,interval_s,name,value,delta" > "$OUTDIR/ethtool.csv"

HAVE_NSTAT=0; have nstat && HAVE_NSTAT=1
HAVE_ETHTOOL=0; have ethtool && HAVE_ETHTOOL=1
HAVE_SS=0; have ss && HAVE_SS=1
[ $HAVE_NSTAT -eq 1 ] || log_event "nstat がないため udpip.csv は取得不可（NA）"
[ $HAVE_ETHTOOL -eq 1 ] || log_event "ethtool がないため ethtool.csv は取得不可"
[ $HAVE_SS -eq 1 ] || log_event "ss がないためソケット情報は取得不可"

PREV_UPTIME=""

sample() {
    local utc up interval k line row name val
    utc=$(utc_now)
    up=$(uptime_now)
    if [ -n "$PREV_UPTIME" ]; then
        interval=$(awk -v a="$up" -v b="$PREV_UPTIME" 'BEGIN{printf "%.2f", a-b}')
    else
        interval=NA
    fi
    CUR=()

    # インターフェース全体
    for k in "${IFACE_KEYS[@]}"; do
        val=$(cat "/sys/class/net/$IFACE/statistics/$k" 2>/dev/null) && [[ "$val" =~ ^[0-9]+$ ]] \
            && CUR["if/$k"]=$val
    done
    row="$utc,$up,$interval"
    for k in "${IFACE_KEYS[@]}"; do row+=",${CUR[if/$k]:-NA},$(delta "if/$k")"; done
    echo "$row" >> "$OUTDIR/iface.csv"
    { echo "### $utc uptime=$up"; ip -s -s link show dev "$IFACE" 2>&1; } >> "$OUTDIR/raw/ip_link.log"

    # UDP/IP（nstatの履歴は更新しない: -s）
    if [ $HAVE_NSTAT -eq 1 ]; then
        while read -r name val _; do
            [[ "$val" =~ ^[0-9]+$ ]] && CUR["ns/$name"]=$val
        done < <(nstat -asz 2>/dev/null | grep -v '^#')
    fi
    row="$utc,$up,$interval"
    for k in "${NSTAT_KEYS[@]}"; do row+=",${CUR[ns/$k]:-NA},$(delta "ns/$k")"; done
    echo "$row" >> "$OUTDIR/udpip.csv"

    # NIC固有統計
    if [ $HAVE_ETHTOOL -eq 1 ]; then
        local raw
        raw=$(ethtool -S "$IFACE" 2>&1)
        { echo "### $utc uptime=$up"; echo "$raw"; } >> "$OUTDIR/raw/ethtool_S.log"
        while IFS=$'\t' read -r name val; do
            [ -n "$name" ] || continue
            CUR["et/$name"]=$val
            echo "$utc,$up,$interval,\"${name//\"/\"\"}\",$val,$(delta "et/$name")" >> "$OUTDIR/ethtool.csv"
        done < <(sed -n 's/^[[:space:]]*\(.*[^[:space:]]\):[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1\t\2/p' <<<"$raw")
    fi

    # UDPソケット（受信キュー・メモリー・ソケットDROP）
    if [ $HAVE_SS -eq 1 ]; then
        {
            echo "### $utc uptime=$up"
            if [ -n "$PORT" ]; then
                ss -u -a -n -m "( sport = :$PORT )" 2>&1
            else
                ss -u -a -n -m 2>&1
            fi
        } >> "$OUTDIR/raw/ss.log"
    fi

    # CPUごとの受信処理統計
    { echo "### $utc uptime=$up"; cat /proc/net/softnet_stat; } >> "$OUTDIR/raw/softnet_stat.log"

    PREV=()
    for k in "${!CUR[@]}"; do PREV[$k]=${CUR[$k]}; done
    PREV_UPTIME=$up
}

# ---------------------------------------------------------------- 補助ツール（別プロセス）
BG_PIDS=()
if have sar; then
    sar -n DEV,EDEV "$INTERVAL" > "$OUTDIR/raw/sar.log" 2>&1 &
    BG_PIDS+=($!)
else
    log_event "sar がないため取得不可（sysstat パッケージ）"
fi
if have mpstat; then
    mpstat -P ALL "$INTERVAL" > "$OUTDIR/raw/mpstat.log" 2>&1 &
    BG_PIDS+=($!)
else
    log_event "mpstat がないため取得不可（sysstat パッケージ）"
fi

STOP=0
trap 'STOP=1' INT TERM

log_event "start host=$HOST iface=$IFACE interval=${INTERVAL}s duration=${DURATION}s"
echo "Monitoring: $OUTDIR (Ctrl+C to stop)" >&2

START_NS=$(date +%s%N)
N=0
while :; do
    sample
    N=$((N + 1))
    [ "$STOP" -eq 1 ] && break
    NOW_NS=$(date +%s%N)
    if [ "$DURATION" -gt 0 ] && [ $((NOW_NS - START_NS)) -ge $((DURATION * 1000000000)) ]; then
        break
    fi
    # 開始時刻からの整数倍の時刻に合わせて待機する（ずれの累積を抑える）
    NEXT_NS=$((START_NS + N * INTERVAL * 1000000000))
    if [ "$DURATION" -gt 0 ] && [ "$NEXT_NS" -gt $((START_NS + DURATION * 1000000000)) ]; then
        NEXT_NS=$((START_NS + DURATION * 1000000000))
    fi
    WAIT_NS=$((NEXT_NS - NOW_NS))
    if [ "$WAIT_NS" -gt 0 ]; then
        sleep "$(awk -v n="$WAIT_NS" 'BEGIN{printf "%.3f", n/1e9}')" &
        wait $! 2>/dev/null
    else
        log_event "採取が間隔より遅れました（$(( -WAIT_NS / 1000000 ))ms）"
    fi
    if [ "$STOP" -eq 1 ]; then
        sample   # 終了要求時の最後の採取
        N=$((N + 1))
        break
    fi
done

for p in "${BG_PIDS[@]}"; do kill -INT "$p" 2>/dev/null; done
wait 2>/dev/null
log_event "stop samples=$N"
echo "end_utc: $(utc_now)" >> "$OUTDIR/meta.txt"
echo "Monitoring finished: $OUTDIR" >&2
