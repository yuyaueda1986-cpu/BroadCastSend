#!/bin/bash
# broadcast_send の --check-config と入力エラー時の終了コードを確認する
set -u
cd "$(dirname "$0")/.."
BIN=./broadcast_send
TMP=$(mktemp -d "${TMPDIR:-/tmp}/bcs_cli_XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail=0

expect() { # expect <exit> <grep-pattern> <args...>
    local want=$1 pat=$2; shift 2
    local out rc
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" -ne "$want" ] || ! grep -q -- "$pat" <<<"$out"; then
        echo "FAIL: $* -> rc=$rc (want $want), pattern '$pat'"; echo "$out" | sed 's/^/    /'
        fail=1
    fi
}

cp examples/send.ini examples/payload.hex "$TMP/"
expect 0 "設定とペイロードの静的検証: OK" $BIN --config "$TMP/send.ini" --check-config
expect 0 "payload .*(hex, 40 bytes)" $BIN --config "$TMP/send.ini" --check-config
expect 0 "planned_pps          : 10000.0" $BIN --config "$TMP/send.ini" --check-config

# 65,507バイトは受理、65,508バイトは拒否
head -c 65507 /dev/urandom > "$TMP/max.bin"
head -c 65508 /dev/urandom > "$TMP/over.bin"
sed -e 's/^format = hex/format = binary/' -e 's/^file = payload.hex/file = max.bin/' "$TMP/send.ini" > "$TMP/max.ini"
sed -e 's/^format = hex/format = binary/' -e 's/^file = payload.hex/file = over.bin/' "$TMP/send.ini" > "$TMP/over.ini"
expect 0 "65507 bytes" $BIN --config "$TMP/max.ini" --check-config
expect 0 "1Gbpsを超える" $BIN --config "$TMP/max.ini" --check-config
expect 1 "65507バイトを超え" $BIN --config "$TMP/over.ini" --check-config

# 入力異常では送信を開始しない（終了コード1、送信開始の表示なし）
sed 's/^period_us = 100/period_us = 50/' "$TMP/send.ini" > "$TMP/bad.ini"
expect 1 "bad.ini:23: period_us" $BIN --config "$TMP/bad.ini"
if $BIN --config "$TMP/bad.ini" 2>&1 | grep -q "送信開始"; then echo "FAIL: started sending with bad config"; fail=1; fi

# 存在しないNICでは送信を開始しない
sed 's/^interface = .*/interface = nonexist0/' "$TMP/send.ini" > "$TMP/nonic.ini"
expect 1 "インターフェース'nonexist0'がありません" $BIN --config "$TMP/nonic.ini"

expect 1 "使い方" $BIN
expect 1 "使い方" $BIN --config "$TMP/send.ini" extra
expect 1 "開けません" $BIN --config "$TMP/none.ini"

[ $fail -eq 0 ] && echo "test_cli: OK"
exit $fail
