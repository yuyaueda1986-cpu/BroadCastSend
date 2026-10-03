#define _GNU_SOURCE
#include "config.h"
#include "payload.h"
#include "text.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define LINE_MAX_LEN 4096

typedef enum { SEC_NONE, SEC_NETWORK, SEC_PAYLOAD, SEC_SEND, SEC_STATS } section_t;

typedef struct {
    section_t section;
    const char *section_name;
    const char *name;
    int required;
} key_def_t;

static const key_def_t KEYS[K_COUNT] = {
    [K_INTERFACE]          = { SEC_NETWORK, "network", "interface", 1 },
    [K_SOURCE_IP]          = { SEC_NETWORK, "network", "source_ip", 1 },
    [K_SOURCE_PORT]        = { SEC_NETWORK, "network", "source_port", 1 },
    [K_BROADCAST_IP]       = { SEC_NETWORK, "network", "broadcast_ip", 1 },
    [K_DESTINATION_PORT]   = { SEC_NETWORK, "network", "destination_port", 1 },
    [K_FORMAT]             = { SEC_PAYLOAD, "payload", "format", 1 },
    [K_PAYLOAD_FILE]       = { SEC_PAYLOAD, "payload", "file", 1 },
    [K_PAYLOAD_SIZE]       = { SEC_PAYLOAD, "payload", "size", 0 },
    [K_PERIOD_US]          = { SEC_SEND, "send", "period_us", 1 },
    [K_PACKETS_PER_CYCLE]  = { SEC_SEND, "send", "packets_per_cycle", 1 },
    [K_DURATION_SEC]       = { SEC_SEND, "send", "duration_sec", 0 },
    [K_MAX_ATTEMPTS]       = { SEC_SEND, "send", "max_attempts", 0 },
    [K_STATS_INTERVAL_SEC] = { SEC_STATS, "stats", "interval_sec", 0 },
    [K_STATS_FILE]         = { SEC_STATS, "stats", "file", 0 },
};

static void set_err(char *err, size_t errlen, const char *path, int line, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (line > 0)
        snprintf(err, errlen, "%s:%d: %s", text_display_path(path), line, msg);
    else
        snprintf(err, errlen, "%s: %s", text_display_path(path), msg);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = '\0';
    return s;
}

/* 符号なし10進数だけを受け付ける（符号・空白・16進表記は不可）。 */
static int parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
    if (*s == '\0')
        return -1;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return -1;
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    if (v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static int parse_ipv4(const char *s, struct in_addr *out)
{
    return inet_pton(AF_INET, s, out) == 1 ? 0 : -1;
}

int resolve_relative(const char *base_file, const char *path, char *out, size_t outlen)
{
    int n;
    if (path[0] == '/') {
        n = snprintf(out, outlen, "%s", path);
    } else {
        const char *slash = strrchr(base_file, '/');
        if (slash == NULL)
            n = snprintf(out, outlen, "%s", path);
        else
            n = snprintf(out, outlen, "%.*s/%s", (int)(slash - base_file), base_file, path);
    }
    return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

static section_t section_from_name(const char *name)
{
    if (strcmp(name, "network") == 0) return SEC_NETWORK;
    if (strcmp(name, "payload") == 0) return SEC_PAYLOAD;
    if (strcmp(name, "send") == 0) return SEC_SEND;
    if (strcmp(name, "stats") == 0) return SEC_STATS;
    return SEC_NONE;
}

static int apply_value(key_id_t id, const char *val, const char *cfg_path, config_t *cfg,
                       char *msg, size_t msglen)
{
    uint64_t u;
    char native[PATH_MAX];
    switch (id) {
    case K_INTERFACE:
        if (strlen(val) >= IF_NAMESIZE) {
            snprintf(msg, msglen, "interface: 名前が長すぎます（最大%d文字）", IF_NAMESIZE - 1);
            return -1;
        }
        strcpy(cfg->interface, val);
        return 0;
    case K_SOURCE_IP:
        if (parse_ipv4(val, &cfg->source_ip) < 0) {
            snprintf(msg, msglen, "source_ip: IPv4アドレスではありません: '%s'", val);
            return -1;
        }
        return 0;
    case K_BROADCAST_IP:
        if (parse_ipv4(val, &cfg->broadcast_ip) < 0) {
            snprintf(msg, msglen, "broadcast_ip: IPv4アドレスではありません: '%s'", val);
            return -1;
        }
        return 0;
    case K_SOURCE_PORT:
        if (parse_u64(val, 0, 65535, &u) < 0) {
            snprintf(msg, msglen, "source_port: 0〜65535の整数を指定してください: '%s'", val);
            return -1;
        }
        cfg->source_port = (uint16_t)u;
        return 0;
    case K_DESTINATION_PORT:
        if (parse_u64(val, 1, 65535, &u) < 0) {
            snprintf(msg, msglen, "destination_port: 1〜65535の整数を指定してください: '%s'", val);
            return -1;
        }
        cfg->destination_port = (uint16_t)u;
        return 0;
    case K_FORMAT:
        if (strcmp(val, "binary") == 0)
            cfg->payload_format = PAYLOAD_BINARY;
        else if (strcmp(val, "hex") == 0)
            cfg->payload_format = PAYLOAD_HEX;
        else if (strcmp(val, "zero") == 0)
            cfg->payload_format = PAYLOAD_ZERO;
        else {
            snprintf(msg, msglen, "format: 'zero'（0データ）、'binary'または'hex'を指定してください: '%s'", val);
            return -1;
        }
        return 0;
    case K_PAYLOAD_FILE:
        if (text_native_path(val, native, sizeof(native)) < 0 ||
            resolve_relative(cfg_path, native, cfg->payload_path, sizeof(cfg->payload_path)) < 0) {
            snprintf(msg, msglen, "file: パスが長すぎるか、ファイル名の文字コードへ変換できません");
            return -1;
        }
        return 0;
    case K_PAYLOAD_SIZE:
        if (parse_u64(val, 1, BCS_PAYLOAD_MAX, &u) < 0) {
            snprintf(msg, msglen, "size: 1〜%uの整数（バイト数）を指定してください: '%s'", BCS_PAYLOAD_MAX, val);
            return -1;
        }
        cfg->payload_size = (size_t)u;
        return 0;
    case K_PERIOD_US:
        if (parse_u64(val, BCS_PERIOD_US_MIN, BCS_PERIOD_US_MAX, &cfg->period_us) < 0) {
            snprintf(msg, msglen, "period_us: %llu〜%lluの整数を指定してください: '%s'",
                     BCS_PERIOD_US_MIN, BCS_PERIOD_US_MAX, val);
            return -1;
        }
        return 0;
    case K_PACKETS_PER_CYCLE:
        if (parse_u64(val, 1, BCS_PACKETS_PER_CYCLE_MAX, &cfg->packets_per_cycle) < 0) {
            snprintf(msg, msglen, "packets_per_cycle: 1〜%lluの整数を指定してください: '%s'",
                     BCS_PACKETS_PER_CYCLE_MAX, val);
            return -1;
        }
        return 0;
    case K_DURATION_SEC:
        if (parse_u64(val, 0, BCS_DURATION_SEC_MAX, &cfg->duration_sec) < 0) {
            snprintf(msg, msglen, "duration_sec: 0〜%lluの整数を指定してください: '%s'",
                     BCS_DURATION_SEC_MAX, val);
            return -1;
        }
        return 0;
    case K_MAX_ATTEMPTS:
        if (parse_u64(val, 0, UINT64_MAX, &cfg->max_attempts) < 0) {
            snprintf(msg, msglen, "max_attempts: 0以上の整数を指定してください: '%s'", val);
            return -1;
        }
        return 0;
    case K_STATS_INTERVAL_SEC:
        if (parse_u64(val, 0, BCS_STATS_INTERVAL_SEC_MAX, &cfg->stats_interval_sec) < 0) {
            snprintf(msg, msglen, "interval_sec: 0〜%lluの整数を指定してください: '%s'",
                     BCS_STATS_INTERVAL_SEC_MAX, val);
            return -1;
        }
        return 0;
    case K_STATS_FILE:
        if (text_native_path(val, native, sizeof(native)) < 0 ||
            resolve_relative(cfg_path, native, cfg->stats_path, sizeof(cfg->stats_path)) < 0) {
            snprintf(msg, msglen, "file: パスが長すぎるか、ファイル名の文字コードへ変換できません");
            return -1;
        }
        return 0;
    default:
        snprintf(msg, msglen, "内部エラー");
        return -1;
    }
}

void config_init(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->stats_interval_sec = 1;
}

const char *config_key_name(key_id_t key) { return KEYS[key].name; }

int config_set(config_t *cfg, key_id_t key, const char *val, const char *base,
               char *err, size_t errlen)
{
    if (apply_value(key, val, base, cfg, err, errlen) < 0) return -1;
    cfg->present |= 1u << key;
    return 0;
}

static int load(const char *path, const char *encoding, int partial,
                config_t *cfg, char *err, size_t errlen)
{
    config_init(cfg);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        int missing = errno == ENOENT;
        set_err(err, errlen, path, 0, "開けません: %s", strerror(errno));
        return partial && missing ? -2 : -1;
    }

    int seen_line[K_COUNT] = {0};
    section_t sec = SEC_NONE;
    char *buf = NULL;
    char *decoded = NULL;
    size_t cap = 0;
    ssize_t nread;
    int lineno = 0;
    int rc = -1;

    errno = 0;
    while ((nread = getline(&buf, &cap, fp)) != -1) {
        lineno++;
        if ((size_t)nread > LINE_MAX_LEN) {
            set_err(err, errlen, path, lineno, "行が長すぎます（最大%d文字）", LINE_MAX_LEN);
            goto out;
        }
        if (memchr(buf, '\0', (size_t)nread) != NULL) {
            set_err(err, errlen, path, lineno, "NUL文字を含んでいます");
            goto out;
        }
        char *raw = buf;
        if (lineno == 1 && (size_t)nread >= 3 && !memcmp(raw, "\xef\xbb\xbf", 3))
            raw += 3;
        char *line = trim(raw);
        if (*line == '\0' || *line == '#' || *line == ';')
            continue;

        free(decoded);
        decoded = NULL;
        char decode_err[256];
        if (text_decode(line, encoding, &decoded, decode_err, sizeof(decode_err)) < 0) {
            set_err(err, errlen, path, lineno, "%s", decode_err);
            goto out;
        }
        line = decoded;

        if (*line == '[') {
            char *close = strchr(line, ']');
            if (close == NULL || *trim(close + 1) != '\0') {
                set_err(err, errlen, path, lineno, "セクション行の形式が不正です: '%s'", line);
                goto out;
            }
            *close = '\0';
            char *name = trim(line + 1);
            sec = section_from_name(name);
            if (sec == SEC_NONE) {
                set_err(err, errlen, path, lineno, "未定義のセクションです: '%s'", name);
                goto out;
            }
            continue;
        }

        char *eq = strchr(line, '=');
        if (eq == NULL) {
            set_err(err, errlen, path, lineno, "'キー = 値'の形式ではありません: '%s'", line);
            goto out;
        }
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (sec == SEC_NONE) {
            set_err(err, errlen, path, lineno, "セクションの前にキーがあります: '%s'", key);
            goto out;
        }
        if (*key == '\0') {
            set_err(err, errlen, path, lineno, "キー名が空です");
            goto out;
        }

        int id = -1;
        for (int i = 0; i < K_COUNT; i++) {
            if (KEYS[i].section == sec && strcmp(KEYS[i].name, key) == 0) {
                id = i;
                break;
            }
        }
        if (id < 0) {
            set_err(err, errlen, path, lineno, "未定義のキーです: '%s'", key);
            goto out;
        }
        if (seen_line[id]) {
            set_err(err, errlen, path, lineno, "キーが重複しています: [%s] %s（最初の定義は%d行目）",
                    KEYS[id].section_name, key, seen_line[id]);
            goto out;
        }
        seen_line[id] = lineno;
        if (*val == '\0') {
            if (partial) continue;
            set_err(err, errlen, path, lineno, "値が空です: '%s'", key);
            goto out;
        }

        char msg[400];
        if (config_set(cfg, (key_id_t)id, val, path, msg, sizeof(msg)) < 0) {
            set_err(err, errlen, path, lineno, "%s", msg);
            goto out;
        }
    }
    if (ferror(fp)) {
        set_err(err, errlen, path, 0, "読込みエラー: %s", strerror(errno));
        goto out;
    }

    rc = partial ? 0 : config_require(cfg, path, err, errlen);
out:
    free(decoded);
    free(buf);
    fclose(fp);
    return rc;
}

int config_require(const config_t *cfg, const char *path, char *err, size_t errlen)
{
    for (int i = 0; i < K_COUNT; i++) {
        int required = KEYS[i].required;
        if (i == K_PAYLOAD_FILE) required = cfg->payload_format != PAYLOAD_ZERO;
        if (i == K_PAYLOAD_SIZE) required = cfg->payload_format == PAYLOAD_ZERO;
        if (required && !CONFIG_HAS(cfg, i)) {
            set_err(err, errlen, path, 0, "必須項目がありません: [%s] %s",
                    KEYS[i].section_name, KEYS[i].name);
            return -1;
        }
    }
    return 0;
}

int config_load(const char *path, config_t *cfg, char *err, size_t errlen)
{
    return load(path, "auto", 0, cfg, err, errlen);
}

int config_load_partial(const char *path, const char *encoding, config_t *cfg,
                        char *err, size_t errlen)
{
    return load(path, encoding, 1, cfg, err, errlen);
}
