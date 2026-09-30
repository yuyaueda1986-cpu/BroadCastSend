#ifndef BCS_CONFIG_H
#define BCS_CONFIG_H

#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define BCS_PERIOD_US_MIN 100ULL
#define BCS_PERIOD_US_MAX 3600000000ULL      /* 1時間 */
#define BCS_PACKETS_PER_CYCLE_MAX 1000000ULL
#define BCS_DURATION_SEC_MAX 315360000ULL    /* 10年 */
#define BCS_STATS_INTERVAL_SEC_MAX 86400ULL

typedef enum {
    PAYLOAD_BINARY,
    PAYLOAD_HEX
} payload_format_t;

typedef struct {
    /* [network] */
    char interface[IF_NAMESIZE];
    struct in_addr source_ip;
    uint16_t source_port;            /* 0はOSによる自動割当 */
    struct in_addr broadcast_ip;
    uint16_t destination_port;

    /* [payload] */
    payload_format_t payload_format;
    char payload_path[PATH_MAX];     /* コンフィグのディレクトリで解決済み */

    /* [send] */
    uint64_t period_us;
    uint64_t packets_per_cycle;
    uint64_t duration_sec;           /* 0は制限なし */
    uint64_t max_attempts;           /* 0は制限なし */

    /* [stats] */
    uint64_t stats_interval_sec;     /* 0は終了時の集計だけ */
    char stats_path[PATH_MAX];       /* 空文字列はCSV出力なし */
} config_t;

/* コンフィグを読み込み、検証する。失敗時は-1を返し、errに行番号付きのメッセージを入れる。 */
int config_load(const char *path, config_t *cfg, char *err, size_t errlen);

/* baseのディレクトリを基準に相対パスを解決する。絶対パスはそのままコピーする。 */
int resolve_relative(const char *base_file, const char *path, char *out, size_t outlen);

#endif
