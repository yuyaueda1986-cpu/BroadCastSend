#define _GNU_SOURCE
#include "stats.h"
#include "text.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "timeutil.h"

static void counters_reset(counters_t *c)
{
    memset(c, 0, sizeof(*c));
    c->gap_min_ns = -1;
}

void stats_init(stats_t *st, uint64_t packets_per_cycle, int64_t start_mono_ns)
{
    memset(st, 0, sizeof(*st));
    counters_reset(&st->total);
    counters_reset(&st->interval);
    st->packets_per_cycle = packets_per_cycle;
    st->start_mono_ns = start_mono_ns;
    st->last_emit_mono_ns = start_mono_ns;
    st->prev_cycle_start_ns = -1;
}

static const char CSV_HEADER[] =
    "record,utc_time,elapsed_s,interval_s,cycles,skipped_cycles,skipped_packets,"
    "attempts,ok,fail,fail_eagain,fail_enobufs,fail_other,ok_payload_bytes,"
    "ok_pps,ok_payload_bytes_per_s,late_avg_us,late_max_us,"
    "cycle_gap_min_us,cycle_gap_max_us,burst_max_us\n";

int stats_open_csv(stats_t *st, const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            snprintf(err, errlen, "%s: 既に存在します（上書きしません）", text_display_path(path));
        else
            snprintf(err, errlen, "%s: 作成できません: %s", text_display_path(path), strerror(errno));
        return -1;
    }
    st->csv = fdopen(fd, "w");
    if (st->csv == NULL) {
        snprintf(err, errlen, "%s: fdopen: %s", text_display_path(path), strerror(errno));
        close(fd);
        return -1;
    }
    fputs(CSV_HEADER, st->csv);
    fflush(st->csv);
    return 0;
}

static void add_both(stats_t *st, size_t off, uint64_t v)
{
    *(uint64_t *)((char *)&st->total + off) += v;
    *(uint64_t *)((char *)&st->interval + off) += v;
}

#define ADD(st, field, v) add_both((st), offsetof(counters_t, field), (v))

static void update_max(int64_t *m, int64_t v) { if (v > *m) *m = v; }
static void update_min(int64_t *m, int64_t v) { if (*m < 0 || v < *m) *m = v; }

void stats_cycle_start(stats_t *st, int64_t start_ns, int64_t lateness_ns, uint64_t skipped)
{
    ADD(st, cycles, 1);
    ADD(st, skipped_cycles, skipped);
    if (lateness_ns < 0)
        lateness_ns = 0;
    ADD(st, late_sum_ns, (uint64_t)lateness_ns);
    update_max(&st->total.late_max_ns, lateness_ns);
    update_max(&st->interval.late_max_ns, lateness_ns);
    if (st->prev_cycle_start_ns >= 0) {
        int64_t gap = start_ns - st->prev_cycle_start_ns;
        ADD(st, gap_count, 1);
        update_min(&st->total.gap_min_ns, gap);
        update_min(&st->interval.gap_min_ns, gap);
        update_max(&st->total.gap_max_ns, gap);
        update_max(&st->interval.gap_max_ns, gap);
    }
    st->prev_cycle_start_ns = start_ns;
}

void stats_skip(stats_t *st, uint64_t skipped)
{
    ADD(st, skipped_cycles, skipped);
}

void stats_send_result(stats_t *st, int err, uint64_t bytes)
{
    ADD(st, attempts, 1);
    if (err == 0) {
        ADD(st, ok, 1);
        ADD(st, ok_bytes, bytes);
        return;
    }
    ADD(st, fail, 1);
    int idx = (err > 0 && err < STATS_ERRNO_MAX) ? err : STATS_ERRNO_MAX - 1;
    st->total.fail_by_errno[idx]++;
    st->interval.fail_by_errno[idx]++;
}

void stats_burst_end(stats_t *st, int64_t duration_ns)
{
    update_max(&st->total.burst_max_ns, duration_ns);
    update_max(&st->interval.burst_max_ns, duration_ns);
}

static double us(int64_t ns) { return (double)ns / 1000.0; }

static void fmt_opt_us(char *buf, size_t len, int64_t ns)
{
    if (ns < 0)
        snprintf(buf, len, "NA");
    else
        snprintf(buf, len, "%.3f", us(ns));
}

static uint64_t fail_eagain(const counters_t *c)
{
    uint64_t v = c->fail_by_errno[EAGAIN];
#if EWOULDBLOCK != EAGAIN
    v += c->fail_by_errno[EWOULDBLOCK];
#endif
    return v;
}

static void write_row(stats_t *st, FILE *fp, const char *record, const counters_t *c,
                      int64_t now_mono_ns, int64_t span_ns)
{
    char utc[64];
    format_utc_now(utc, sizeof(utc));
    double span_s = (double)span_ns / 1e9;
    double pps = span_s > 0 ? (double)c->ok / span_s : 0.0;
    double bps = span_s > 0 ? (double)c->ok_bytes / span_s : 0.0;
    uint64_t eagain = fail_eagain(c);
    uint64_t enobufs = c->fail_by_errno[ENOBUFS];
    char late_avg[32], late_max[32], gmin[32], gmax[32], bmax[32];
    if (c->cycles > 0) {
        snprintf(late_avg, sizeof(late_avg), "%.3f", us((int64_t)(c->late_sum_ns / c->cycles)));
        fmt_opt_us(late_max, sizeof(late_max), c->late_max_ns);
        fmt_opt_us(bmax, sizeof(bmax), c->burst_max_ns);
    } else {
        snprintf(late_avg, sizeof(late_avg), "NA");
        snprintf(late_max, sizeof(late_max), "NA");
        snprintf(bmax, sizeof(bmax), "NA");
    }
    if (c->gap_count > 0) {
        fmt_opt_us(gmin, sizeof(gmin), c->gap_min_ns);
        fmt_opt_us(gmax, sizeof(gmax), c->gap_max_ns);
    } else {
        snprintf(gmin, sizeof(gmin), "NA");
        snprintf(gmax, sizeof(gmax), "NA");
    }
    fprintf(fp,
            "%s,%s,%.6f,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f,%.1f,%s,%s,%s,%s,%s\n",
            record, utc, (double)(now_mono_ns - st->start_mono_ns) / 1e9, span_s,
            c->cycles, c->skipped_cycles, c->skipped_cycles * st->packets_per_cycle,
            c->attempts, c->ok, c->fail, eagain, enobufs, c->fail - eagain - enobufs,
            c->ok_bytes, pps, bps, late_avg, late_max, gmin, gmax, bmax);
}

static void print_interval_line(stats_t *st, const counters_t *c, int64_t now_mono_ns, int64_t span_ns)
{
    double span_s = (double)span_ns / 1e9;
    ui_printf("[%9.3fs] cycles=%" PRIu64 " skipped=%" PRIu64 " attempts=%" PRIu64 " ok=%" PRIu64
           " fail=%" PRIu64 " ok_pps=%.1f ok_Mbps(payload)=%.3f late_max_us=%.1f\n",
           (double)(now_mono_ns - st->start_mono_ns) / 1e9, c->cycles, c->skipped_cycles,
           c->attempts, c->ok, c->fail, span_s > 0 ? (double)c->ok / span_s : 0.0,
           span_s > 0 ? (double)c->ok_bytes * 8.0 / span_s / 1e6 : 0.0,
           c->cycles > 0 ? us(c->late_max_ns) : 0.0);
    fflush(stdout);
}

void stats_emit_interval(stats_t *st, int64_t now_mono_ns)
{
    int64_t span = now_mono_ns - st->last_emit_mono_ns;
    if (st->csv != NULL) {
        write_row(st, st->csv, "interval", &st->interval, now_mono_ns, span);
        fflush(st->csv);
    } else if (st->print_intervals) {
        print_interval_line(st, &st->interval, now_mono_ns, span);
    }
    counters_reset(&st->interval);
    st->last_emit_mono_ns = now_mono_ns;
}

void stats_finish(stats_t *st, int64_t now_mono_ns, const char *reason)
{
    const counters_t *c = &st->total;
    int64_t span = now_mono_ns - st->start_mono_ns;
    double span_s = (double)span / 1e9;

    if (st->csv != NULL) {
        write_row(st, st->csv, "total", c, now_mono_ns, span);
        fclose(st->csv);
        st->csv = NULL;
    }

    char utc[64];
    format_utc_now(utc, sizeof(utc));
    ui_printf("=== 送信結果 ===\n");
    ui_printf("end_utc              : %s\n", utc);
    ui_printf("end_reason           : %s\n", reason);
    ui_printf("elapsed_s            : %.6f\n", span_s);
    ui_printf("cycles_executed      : %" PRIu64 "\n", c->cycles);
    ui_printf("skipped_cycles       : %" PRIu64 "\n", c->skipped_cycles);
    ui_printf("skipped_packets      : %" PRIu64 "  (skipped_cycles x packets_per_cycle, 未送信相当)\n",
           c->skipped_cycles * st->packets_per_cycle);
    ui_printf("send_attempts        : %" PRIu64 "\n", c->attempts);
    ui_printf("send_ok (API accept) : %" PRIu64 "\n", c->ok);
    ui_printf("send_fail            : %" PRIu64 "\n", c->fail);
    for (int e = 1; e < STATS_ERRNO_MAX; e++) {
        if (c->fail_by_errno[e] == 0)
            continue;
        if (e == STATS_ERRNO_MAX - 1)
            ui_printf("  errno>=%d          : %" PRIu64 "\n", e, c->fail_by_errno[e]);
        else
            ui_printf("  errno %3d %-10s : %" PRIu64 "  (%s)\n", e, errno_name(e), c->fail_by_errno[e],
                   strerror(e));
    }
    ui_printf("ok_payload_bytes     : %" PRIu64 "\n", c->ok_bytes);
    if (span_s > 0) {
        ui_printf("ok_pps (avg)         : %.1f\n", (double)c->ok / span_s);
        ui_printf("ok_payload_Mbps (avg): %.3f\n", (double)c->ok_bytes * 8.0 / span_s / 1e6);
    }
    if (c->cycles > 0) {
        ui_printf("late_avg_us          : %.3f\n", us((int64_t)(c->late_sum_ns / c->cycles)));
        ui_printf("late_max_us          : %.3f\n", us(c->late_max_ns));
        ui_printf("burst_max_us         : %.3f\n", us(c->burst_max_ns));
    }
    if (c->gap_count > 0) {
        ui_printf("cycle_gap_min_us     : %.3f\n", us(c->gap_min_ns));
        ui_printf("cycle_gap_max_us     : %.3f\n", us(c->gap_max_ns));
    }
    fflush(stdout);
}
