#ifndef BCS_CYCLE_SCHED_H
#define BCS_CYCLE_SCHED_H

#include <stdint.h>

/*
 * 周期スケジュールの計算（時刻は引数で与えるため、実時計なしでテストできる）。
 *
 * 周期kの予定時刻は start_ns + k * period_ns。
 * - 起床が予定から1周期未満の遅れなら、その周期を実行する。
 * - 起床が複数周期遅れたら、過ぎた周期をスキップし、現在の周期を1回だけ実行する。
 * - バースト終了時に次の予定時刻を過ぎていたら、その周期をスキップして未到来の周期へ進む。
 * - 終了時刻（end_ns）以降の予定周期はスキップ数に含めない。
 */

#define SCHED_NO_END INT64_MAX

typedef struct {
    int64_t start_ns;
    int64_t period_ns;
    int64_t end_ns;        /* SCHED_NO_ENDなら終了時刻なし */
    uint64_t end_idx;      /* 予定時刻がend_ns以上になる最初の周期番号 */
    uint64_t next_idx;     /* 次に実行を検討する周期番号 */
} sched_t;

typedef enum {
    SCHED_WAIT,    /* 次の予定時刻前 */
    SCHED_RUN,     /* 周期を実行する */
    SCHED_END      /* 終了時刻に到達した */
} sched_action_t;

void sched_init(sched_t *s, int64_t start_ns, int64_t period_ns, int64_t end_ns);

int64_t sched_target(const sched_t *s, uint64_t idx);

/* 次に起床すべき予定時刻 */
static inline int64_t sched_next_target(const sched_t *s) { return sched_target(s, s->next_idx); }

/*
 * nowに起床したときの動作を決める。
 * SCHED_RUN: *run_idxに実行する周期、*latenessに予定時刻からの遅れ（ns）を返す。
 * SCHED_RUN / SCHED_END: *skippedにこの判断で新たにスキップした周期数を返す。
 */
sched_action_t sched_on_wake(sched_t *s, int64_t now, uint64_t *run_idx, int64_t *lateness,
                             uint64_t *skipped);

/* バースト終了時刻nowを与え、超過した予定周期をスキップする。戻り値はスキップ数。 */
uint64_t sched_after_burst(sched_t *s, int64_t now);

#endif
