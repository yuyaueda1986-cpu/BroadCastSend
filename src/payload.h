#ifndef BCS_PAYLOAD_H
#define BCS_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* IPv4全長65,535 - IPv4ヘッダー20 - UDPヘッダー8 */
#define BCS_PAYLOAD_MAX 65507u

typedef struct {
    uint8_t *data;
    size_t len;
} payload_t;

/* ファイル全体を1個のUDPデータ部として読み込む。サイズは1〜BCS_PAYLOAD_MAX。 */
int payload_load(const char *path, payload_format_t format, payload_t *out, char *err, size_t errlen);
/* zeroはファイルを開かず、指定サイズの0x00データをメモリー上に生成する。 */
int payload_prepare(const config_t *cfg, payload_t *out, char *err, size_t errlen);
void payload_free(payload_t *p);

/* hex形式の文字列をデコードする（テスト用に公開）。 */
int payload_decode_hex(const char *path, const char *text, size_t textlen, payload_t *out,
                       char *err, size_t errlen);

#endif
