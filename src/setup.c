#define _GNU_SOURCE
#include "setup.h"
#include "text.h"
#include "payload.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int nic_from_ifaddr(const struct ifaddrs *ifa, nic_t *nic)
{
    if (!ifa->ifa_name || strlen(ifa->ifa_name) >= IF_NAMESIZE ||
        !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
        !ifa->ifa_netmask || !ifa->ifa_broadaddr ||
        ifa->ifa_netmask->sa_family != AF_INET || ifa->ifa_broadaddr->sa_family != AF_INET ||
        (ifa->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST) ||
        (ifa->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) return 0;
    struct in_addr addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
    struct in_addr mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;
    uint32_t hostmask = ~ntohl(mask.s_addr);
    /* /31 and /32 have no subnet broadcast; reject non-contiguous masks too. */
    if (hostmask < 3 || (hostmask & (hostmask + 1)) || addr.s_addr == INADDR_ANY) return 0;
    memset(nic, 0, sizeof(*nic));
    strcpy(nic->name, ifa->ifa_name);
    nic->address = addr;
    nic->netmask = mask;
    nic->broadcast = ((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr;
    nic->flags = ifa->ifa_flags;
    for (uint32_t m = ntohl(mask.s_addr); m; m <<= 1) nic->prefix++;
    return 1;
}

int nic_discover(nic_t **items, size_t *count, char *err, size_t errlen)
{
    *items = NULL;
    *count = 0;
    struct ifaddrs *list;
    if (getifaddrs(&list) < 0) {
        snprintf(err, errlen, "NIC情報を取得できません: getifaddrs: %s", strerror(errno));
        return -1;
    }
    for (const struct ifaddrs *p = list; p; p = p->ifa_next) {
        nic_t nic;
        if (!nic_from_ifaddr(p, &nic)) continue;
        nic_t *new_items = realloc(*items, (*count + 1) * sizeof(nic_t));
        if (!new_items) {
            free(*items); *items = NULL; *count = 0;
            freeifaddrs(list);
            snprintf(err, errlen, "NIC情報: メモリー不足");
            return -1;
        }
        *items = new_items;
        (*items)[(*count)++] = nic;
    }
    freeifaddrs(list);
    return 0;
}

static int read_answer(FILE *input, FILE *output, char **answer, char *err, size_t errlen)
{
    char *raw = NULL;
    size_t cap = 0;
    fflush(output);
    ssize_t n = getline(&raw, &cap, input);
    if (n < 0) {
        free(raw);
        snprintf(err, errlen, "入力が終了しました。設定を完了できないため送信しません");
        return -1;
    }
    if (n > 4096 || memchr(raw, '\0', (size_t)n)) {
        free(raw);
        snprintf(err, errlen, "入力が長すぎるかNUL文字を含んでいます");
        return 1;
    }
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r' ||
                     raw[n - 1] == ' ' || raw[n - 1] == '\t')) raw[--n] = 0;
    char *start = raw;
    while (*start == ' ' || *start == '\t') start++;
    int rc = text_input(start, answer, err, errlen);
    free(raw);
    return rc < 0 ? 1 : 0;
}

static int matches(const config_t *cfg, const nic_t *nic)
{
    return (!CONFIG_HAS(cfg, K_INTERFACE) || !strcmp(cfg->interface, nic->name)) &&
           (!CONFIG_HAS(cfg, K_SOURCE_IP) || cfg->source_ip.s_addr == nic->address.s_addr) &&
           (!CONFIG_HAS(cfg, K_BROADCAST_IP) || cfg->broadcast_ip.s_addr == nic->broadcast.s_addr);
}

int setup_network(config_t *cfg, const nic_t *items, size_t count, FILE *input,
                  FILE *output, char *err, size_t errlen)
{
    if (CONFIG_HAS(cfg, K_INTERFACE) && CONFIG_HAS(cfg, K_SOURCE_IP) &&
        CONFIG_HAS(cfg, K_BROADCAST_IP)) return 0;
    size_t available = 0, selected = 0;
    for (size_t i = 0; i < count; i++) {
        if (matches(cfg, &items[i])) { available++; selected = i; }
    }
    if (!available) {
        snprintf(err, errlen, "設定に合う送信可能なNIC / IPv4がありません。ip -4 addr show でUP・BROADCAST・IPv4を確認してください");
        return -1;
    }
    /* Always ask for an unspecified NIC, including when only one NIC exists. */
    if (!CONFIG_HAS(cfg, K_INTERFACE) || available > 1) {
        if (!input) {
            snprintf(err, errlen, "NIC / IPv4の選択が必要です。端末で起動するか --interactive を指定し、または interface / source_ip を設定してください");
            return -1;
        }
        ui_fprintf(output, "送信先のネットワークに接続したNIC / IPv4を選んでください。\n");
        size_t number = 0;
        for (size_t i = 0; i < count; i++) {
            if (!matches(cfg, &items[i])) continue;
            char addr[INET_ADDRSTRLEN], bc[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &items[i].address, addr, sizeof(addr));
            inet_ntop(AF_INET, &items[i].broadcast, bc, sizeof(bc));
            ui_fprintf(output, "  %zu) %s  IPv4=%s/%u  broadcast=%s  %s\n",
                       ++number, items[i].name, addr, items[i].prefix, bc,
                       items[i].flags & IFF_RUNNING ? "UP/RUNNING" : "UP (リンク未確認)");
        }
        for (;;) {
            ui_fprintf(output, "NIC / IPv4の番号 [1-%zu]: ", available);
            char *answer = NULL;
            int rc = read_answer(input, output, &answer, err, errlen);
            if (rc < 0) return -1;
            if (rc > 0) { ui_fprintf(output, "%s\n", err); continue; }
            char *end;
            errno = 0;
            unsigned long choice = strtoul(answer, &end, 10);
            int valid = answer[0] >= '0' && answer[0] <= '9' && !*end &&
                        !errno && choice >= 1 && choice <= available;
            free(answer);
            if (!valid) { ui_fprintf(output, "候補の番号を入力してください。\n"); continue; }
            for (size_t i = 0, index = 0; i < count; i++) {
                if (matches(cfg, &items[i]) && ++index == choice) { selected = i; break; }
            }
            break;
        }
    }
    const nic_t *nic = &items[selected];
    strcpy(cfg->interface, nic->name);
    cfg->source_ip = nic->address;
    cfg->broadcast_ip = nic->broadcast;
    cfg->present |= (1u << K_INTERFACE) | (1u << K_SOURCE_IP) | (1u << K_BROADCAST_IP);
    char src[INET_ADDRSTRLEN], bc[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->source_ip, src, sizeof(src));
    inet_ntop(AF_INET, &cfg->broadcast_ip, bc, sizeof(bc));
    ui_fprintf(output, "自動補完: interface=%s  source_ip=%s  broadcast_ip=%s\n", cfg->interface, src, bc);
    return 0;
}

int setup_missing(config_t *cfg, FILE *input, FILE *output, char *err, size_t errlen)
{
    static const struct { key_id_t key; const char *label; const char *fallback; } prompts[] = {
        {K_SOURCE_PORT, "送信元ポート (0=OSが自動割当、0-65535)", "0"},
        {K_DESTINATION_PORT, "送信先ポート (受信側と同じ番号、1-65535)", "50000"},
        {K_FORMAT, "ペイロード形式 (zero=0データ送信・ファイル不要、hex=16進数ファイル、binary=ファイルをそのまま)", "zero"},
        {K_PAYLOAD_FILE, "ペイロードファイル (相対パスはカレントディレクトリ基準)", NULL},
        {K_PAYLOAD_SIZE, "0データのサイズ (バイト数、1-65507、全バイト0x00)", "100"},
        {K_PERIOD_US, "送信周期 (マイクロ秒、100-3600000000、1000000=1秒)", "1000000"},
        {K_PACKETS_PER_CYCLE, "1周期に送るパケット数 (1-1000000)", "1"},
        {K_DURATION_SEC, "送信時間 (秒、0=無制限、Ctrl+Cで停止)", "60"},
        {K_MAX_ATTEMPTS, "送信試行数の上限 (0=制限なし)", "0"},
        {K_STATS_INTERVAL_SEC, "統計の表示間隔 (秒、0=終了時のみ)", "1"},
        {K_STATS_FILE, "統計CSVファイル (相対パスはカレント基準、-=作らない)", "-"},
    };
    for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++) {
        key_id_t key = prompts[i].key;
        if (key == K_PAYLOAD_FILE && cfg->payload_format == PAYLOAD_ZERO) continue;
        if (key == K_PAYLOAD_SIZE && cfg->payload_format != PAYLOAD_ZERO) continue;
        if (CONFIG_HAS(cfg, key)) continue;
        for (;;) {
            ui_fprintf(output, "%s (%s)%s%s%s: ", prompts[i].label, config_key_name(key),
                       prompts[i].fallback ? " [" : "", prompts[i].fallback ? prompts[i].fallback : "",
                       prompts[i].fallback ? "]" : "");
            char *answer = NULL;
            int rc = read_answer(input, output, &answer, err, errlen);
            if (rc < 0) return -1;
            if (rc > 0) { ui_fprintf(output, "%s\n", err); continue; }
            const char *value = *answer ? answer : prompts[i].fallback;
            if (!value) { free(answer); ui_fprintf(output, "値を入力してください。\n"); continue; }
            if (key == K_STATS_FILE && !strcmp(value, "-")) {
                cfg->stats_path[0] = 0;
                cfg->present |= 1u << key;
                free(answer);
                break;
            }
            rc = config_set(cfg, key, value, "", err, errlen);
            free(answer);
            if (rc == 0 && key == K_PAYLOAD_FILE) {
                payload_t payload;
                rc = payload_load(cfg->payload_path, cfg->payload_format, &payload, err, errlen);
                if (rc == 0) payload_free(&payload);
                else cfg->present &= ~(1u << key);
            }
            if (rc == 0) break;
            ui_fprintf(output, "%s\n", err);
        }
    }
    return 0;
}
