#ifndef BCS_STATS_H
#define BCS_STATS_H

#include <stdint.h>
#include <stdio.h>

#define STATS_ERRNO_MAX 256

typedef struct {
    uint64_t cycles;           /* 実行した周期数 */
    uint64_t skipped_cycles;   /* スキップした周期数 */
    uint64_t attempts;         /* 送信試行数（成功+失敗） */
    uint64_t ok;               /* 送信API成功数 */
    uint64_t fail;             /* 送信API失敗数 */
    uint64_t ok_bytes;         /* API成功分のUDPデータ部バイト数 */
    uint64_t fail_by_errno[STATS_ERRNO_MAX]; /* 最後の要素はSTATS_ERRNO_MAX-1以上のerrno */
    uint64_t late_sum_ns;      /* 周期開始の遅れの合計 */
    int64_t late_max_ns;
    uint64_t gap_count;        /* 周期開始間隔の標本数 */
    int64_t gap_min_ns;
    int64_t gap_max_ns;
    int64_t burst_max_ns;
} counters_t;

typedef struct {
    counters_t total;
    counters_t interval;
    uint64_t packets_per_cycle;
    int64_t start_mono_ns;
    int64_t last_emit_mono_ns;
    int64_t prev_cycle_start_ns; /* 前回の周期開始時刻（未実行なら-1） */
    FILE *csv;                   /* NULLならCSVなし */
    int print_intervals;         /* CSVがないとき、区間集計を標準出力に出す */
} stats_t;

void stats_init(stats_t *st, uint64_t packets_per_cycle, int64_t start_mono_ns);

/* CSVを新規作成する（既存ファイルは上書きしない）。 */
int stats_open_csv(stats_t *st, const char *path, char *err, size_t errlen);

void stats_cycle_start(stats_t *st, int64_t start_ns, int64_t lateness_ns, uint64_t skipped);
void stats_skip(stats_t *st, uint64_t skipped);
void stats_send_result(stats_t *st, int err, uint64_t bytes);
void stats_burst_end(stats_t *st, int64_t duration_ns);

/* 区間集計を出力し、区間カウンターをリセットする。 */
void stats_emit_interval(stats_t *st, int64_t now_mono_ns);

/* 終了時の集計（CSVのtotal行と標準出力のサマリー） */
void stats_finish(stats_t *st, int64_t now_mono_ns, const char *reason);

#endif
