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
    PAYLOAD_HEX,
    PAYLOAD_ZERO
} payload_format_t;

typedef enum {
    K_INTERFACE, K_SOURCE_IP, K_SOURCE_PORT, K_BROADCAST_IP, K_DESTINATION_PORT,
    K_FORMAT, K_PAYLOAD_FILE, K_PAYLOAD_SIZE,
    K_PERIOD_US, K_PACKETS_PER_CYCLE, K_DURATION_SEC, K_MAX_ATTEMPTS,
    K_STATS_INTERVAL_SEC, K_STATS_FILE, K_COUNT
} key_id_t;

#define CONFIG_HAS(cfg, key) (((cfg)->present & (1u << (key))) != 0)

typedef struct {
    unsigned present;              /* 設定済み項目。0と未指定を区別する */
    /* [network] */
    char interface[IF_NAMESIZE];
    struct in_addr source_ip;
    uint16_t source_port;            /* 0はOSによる自動割当 */
    struct in_addr broadcast_ip;
    uint16_t destination_port;

    /* [payload] */
    payload_format_t payload_format;
    char payload_path[PATH_MAX];     /* コンフィグのディレクトリで解決済み */
    size_t payload_size;             /* zero形式で生成する0x00のバイト数 */

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
void config_init(config_t *cfg);
/* Partial loader: -2 means the file does not exist; -1 means another input error. */
int config_load_partial(const char *path, const char *encoding, config_t *cfg,
                        char *err, size_t errlen);
int config_require(const config_t *cfg, const char *path, char *err, size_t errlen);
int config_set(config_t *cfg, key_id_t key, const char *utf8_value, const char *base,
               char *err, size_t errlen);
const char *config_key_name(key_id_t key);

/* baseのディレクトリを基準に相対パスを解決する。絶対パスはそのままコピーする。 */
int resolve_relative(const char *base_file, const char *path, char *out, size_t outlen);

#endif
