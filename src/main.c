#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "payload.h"
#include "cycle_sched.h"
#include "sender.h"
#include "stats.h"
#include "timeutil.h"

#define EXIT_INPUT_ERROR 1
#define EXIT_RUNTIME_ERROR 2

static volatile sig_atomic_t g_stop_signal = 0;
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    g_stop_signal = sig;
    g_stop = 1;
}

static void usage(FILE *fp, const char *prog)
{
    fprintf(fp,
            "使い方: %s --config FILE [--check-config] [--stats-file FILE]\n"
            "\n"
            "  -c, --config FILE      コンフィグファイル（必須）\n"
            "  -n, --check-config     設定とペイロードを静的に検証して終了する（送信しない）\n"
            "  -s, --stats-file FILE  [stats] file を上書き指定する（カレントディレクトリ基準）\n"
            "  -h, --help             このヘルプを表示する\n"
            "\n"
            "終了コード: 0=正常, 1=入力エラー, 2=実行時エラー\n",
            prog);
}

static void print_config(const config_t *cfg, const payload_t *pl)
{
    char src[INET_ADDRSTRLEN], bc[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->source_ip, src, sizeof(src));
    inet_ntop(AF_INET, &cfg->broadcast_ip, bc, sizeof(bc));

    double pps = (double)cfg->packets_per_cycle * 1e6 / (double)cfg->period_us;
    double bps = (double)pl->len * 8.0 * pps;

    printf("=== 設定 ===\n");
    printf("interface            : %s\n", cfg->interface);
    printf("source               : %s:%u%s\n", src, cfg->source_port,
           cfg->source_port == 0 ? " (auto)" : "");
    printf("destination          : %s:%u\n", bc, cfg->destination_port);
    printf("payload              : %s (%s, %zu bytes)\n", cfg->payload_path,
           cfg->payload_format == PAYLOAD_HEX ? "hex" : "binary", pl->len);
    printf("period_us            : %" PRIu64 "\n", cfg->period_us);
    printf("packets_per_cycle    : %" PRIu64 "\n", cfg->packets_per_cycle);
    printf("duration_sec         : %" PRIu64 "%s\n", cfg->duration_sec,
           cfg->duration_sec == 0 ? " (無制限)" : "");
    printf("max_attempts         : %" PRIu64 "%s\n", cfg->max_attempts,
           cfg->max_attempts == 0 ? " (無制限)" : "");
    printf("stats.interval_sec   : %" PRIu64 "%s\n", cfg->stats_interval_sec,
           cfg->stats_interval_sec == 0 ? " (終了時のみ)" : "");
    printf("stats.file           : %s\n", cfg->stats_path[0] ? cfg->stats_path : "(なし)");
    printf("planned_pps          : %.1f\n", pps);
    printf("planned_payload_Mbps : %.3f  (UDPデータ部のみ。ヘッダー・IP分割・フレーム間隔を含まない)\n",
           bps / 1e6);
    if (bps > 1e9)
        printf("注意: データ部だけで1Gbpsを超える設定です（過負荷試験として送信します）\n");
    fflush(stdout);
}

static void print_socket_info(const config_t *cfg, const sender_t *s, size_t payload_len)
{
    char local[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s->local.sin_addr, local, sizeof(local));
    printf("=== ソケット ===\n");
    printf("ifindex              : %u\n", s->ifindex);
    printf("bound                : %s:%u\n", local, ntohs(s->local.sin_port));
    if (s->mtu > 0) {
        printf("mtu                  : %d\n", s->mtu);
        int max_unfrag = s->mtu - 28;
        printf("ip_fragmentation     : %s (MTU内のUDPデータ部上限 %d bytes)\n",
               (long)payload_len > max_unfrag ? "あり" : "なし", max_unfrag);
    } else {
        printf("mtu                  : 取得不可\n");
    }
    if (s->sndbuf > 0)
        printf("so_sndbuf            : %d\n", s->sndbuf);
    (void)cfg;
    fflush(stdout);
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTARTなし：待機中でも即時に終了要求を確認する */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

typedef enum {
    END_NONE,
    END_DURATION,
    END_MAX_ATTEMPTS,
    END_SIGNAL,
    END_FATAL
} end_reason_t;

static int run(const config_t *cfg, sender_t *snd, size_t payload_len, stats_t *st,
               int64_t start, char *reason, size_t reasonlen)
{
    const int64_t period_ns = (int64_t)cfg->period_us * 1000;
    const int64_t end_ns = cfg->duration_sec ? start + (int64_t)cfg->duration_sec * 1000000000LL
                                             : SCHED_NO_END;
    const int64_t stats_ns = (int64_t)cfg->stats_interval_sec * 1000000000LL;
    int64_t next_stats = stats_ns ? start + stats_ns : INT64_MAX;

    sched_t sc;
    sched_init(&sc, start, period_ns, end_ns);

    end_reason_t why = END_NONE;
    int fatal_errno = 0;

    while (why == END_NONE) {
        if (g_stop) {
            why = END_SIGNAL;
            break;
        }
        int64_t wake = sched_next_target(&sc);
        if (next_stats < wake)
            wake = next_stats;
        if (end_ns < wake)
            wake = end_ns;

        int64_t now = mono_now_ns();
        if (now < wake) {
            struct timespec ts = ns_to_timespec(wake);
            int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            if (r == EINTR)
                continue; /* 終了要求はループ先頭で確認する */
            now = mono_now_ns();
        }

        if (now >= next_stats) {
            stats_emit_interval(st, now);
            while (next_stats <= now)
                next_stats += stats_ns;
        }

        uint64_t idx, skipped;
        int64_t late;
        sched_action_t act = sched_on_wake(&sc, now, &idx, &late, &skipped);
        if (act == SCHED_WAIT)
            continue;
        if (act == SCHED_END) {
            stats_skip(st, skipped);
            why = END_DURATION;
            break;
        }

        stats_cycle_start(st, now, late, skipped);

        int64_t t = now;
        for (uint64_t i = 0; i < cfg->packets_per_cycle; i++) {
            if (g_stop) {
                why = END_SIGNAL;
                break;
            }
            if (cfg->max_attempts && st->total.attempts >= cfg->max_attempts) {
                why = END_MAX_ATTEMPTS;
                break;
            }
            if (i > 0 && end_ns != SCHED_NO_END) {
                t = mono_now_ns();
                if (t >= end_ns) {
                    why = END_DURATION;
                    break;
                }
            }
            int e = sender_send(snd, &g_stop);
            if (e == EINTR) {
                /* 終了要求による中断。試行として数えない */
                why = END_SIGNAL;
                break;
            }
            stats_send_result(st, e, payload_len);
            if (e != 0 && !sender_errno_is_transient(e)) {
                fatal_errno = e;
                why = END_FATAL;
                break;
            }
        }

        int64_t burst_end = mono_now_ns();
        stats_burst_end(st, burst_end - now);
        if (why != END_NONE)
            break;
        if (cfg->max_attempts && st->total.attempts >= cfg->max_attempts) {
            why = END_MAX_ATTEMPTS;
            break;
        }
        stats_skip(st, sched_after_burst(&sc, burst_end));
    }

    switch (why) {
    case END_DURATION:
        snprintf(reason, reasonlen, "duration (%" PRIu64 " s)", cfg->duration_sec);
        break;
    case END_MAX_ATTEMPTS:
        snprintf(reason, reasonlen, "max_attempts (%" PRIu64 ")", cfg->max_attempts);
        break;
    case END_SIGNAL:
        snprintf(reason, reasonlen, "signal (%s)",
                 g_stop_signal == SIGTERM ? "SIGTERM" : g_stop_signal == SIGINT ? "SIGINT" : "?");
        break;
    case END_FATAL:
        snprintf(reason, reasonlen, "fatal send error: errno %d %s (%s)", fatal_errno,
                 errno_name(fatal_errno), strerror(fatal_errno));
        break;
    default:
        snprintf(reason, reasonlen, "unknown");
        break;
    }
    return why == END_FATAL ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *stats_override = NULL;
    int check_only = 0;

    static const struct option opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "check-config", no_argument, NULL, 'n' },
        { "stats-file", required_argument, NULL, 's' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "c:ns:h", opts, NULL)) != -1) {
        switch (c) {
        case 'c': config_path = optarg; break;
        case 'n': check_only = 1; break;
        case 's': stats_override = optarg; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return EXIT_INPUT_ERROR;
        }
    }
    if (optind != argc || config_path == NULL) {
        usage(stderr, argv[0]);
        return EXIT_INPUT_ERROR;
    }

    char err[1024];
    config_t cfg;
    if (config_load(config_path, &cfg, err, sizeof(err)) < 0) {
        fprintf(stderr, "設定エラー: %s\n", err);
        return EXIT_INPUT_ERROR;
    }
    if (stats_override != NULL) {
        int n = snprintf(cfg.stats_path, sizeof(cfg.stats_path), "%s", stats_override);
        if (n < 0 || (size_t)n >= sizeof(cfg.stats_path)) {
            fprintf(stderr, "設定エラー: --stats-file: パスが長すぎます\n");
            return EXIT_INPUT_ERROR;
        }
    }

    payload_t pl;
    if (payload_load(cfg.payload_path, cfg.payload_format, &pl, err, sizeof(err)) < 0) {
        fprintf(stderr, "ペイロードエラー: %s\n", err);
        return EXIT_INPUT_ERROR;
    }

    print_config(&cfg, &pl);
    if (check_only) {
        printf("設定とペイロードの静的検証: OK（NIC・アドレスの検証は通常起動時に行います）\n");
        payload_free(&pl);
        return 0;
    }

    if (install_signals() < 0) {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        payload_free(&pl);
        return EXIT_RUNTIME_ERROR;
    }

    sender_t snd;
    if (sender_open(&snd, &cfg, pl.data, pl.len, err, sizeof(err)) < 0) {
        fprintf(stderr, "ネットワーク設定エラー: %s\n", err);
        payload_free(&pl);
        return EXIT_INPUT_ERROR;
    }
    print_socket_info(&cfg, &snd, pl.len);

    stats_t st;
    stats_init(&st, cfg.packets_per_cycle, 0);
    if (cfg.stats_path[0] != '\0' && stats_open_csv(&st, cfg.stats_path, err, sizeof(err)) < 0) {
        fprintf(stderr, "統計ファイルエラー: %s\n", err);
        sender_close(&snd);
        payload_free(&pl);
        return EXIT_INPUT_ERROR;
    }
    st.print_intervals = (st.csv == NULL);

    char utc[64];
    format_utc_now(utc, sizeof(utc));
    printf("=== 送信開始 ===\nstart_utc            : %s\n", utc);
    fflush(stdout);

    int64_t start = mono_now_ns();
    st.start_mono_ns = start;
    st.last_emit_mono_ns = start;

    char reason[256];
    int rc = run(&cfg, &snd, pl.len, &st, start, reason, sizeof(reason));
    stats_finish(&st, mono_now_ns(), reason);

    sender_close(&snd);
    payload_free(&pl);
    if (rc < 0) {
        fprintf(stderr, "送信を中止しました: %s\n", reason);
        return EXIT_RUNTIME_ERROR;
    }
    return 0;
}
