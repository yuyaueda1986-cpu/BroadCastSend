#include "cycle_sched.h"

/* 予定時刻がt以上になる最初の周期番号（t >= start） */
static uint64_t first_idx_at_or_after(const sched_t *s, int64_t t)
{
    int64_t d = t - s->start_ns;
    if (d <= 0)
        return 0;
    return (uint64_t)((d + s->period_ns - 1) / s->period_ns);
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* [from, to)のうち、終了時刻より前の予定周期の数 */
static uint64_t count_before_end(const sched_t *s, uint64_t from, uint64_t to)
{
    to = min_u64(to, s->end_idx);
    return to > from ? to - from : 0;
}

void sched_init(sched_t *s, int64_t start_ns, int64_t period_ns, int64_t end_ns)
{
    s->start_ns = start_ns;
    s->period_ns = period_ns;
    s->end_ns = end_ns;
    s->end_idx = (end_ns == SCHED_NO_END) ? UINT64_MAX : first_idx_at_or_after(s, end_ns);
    s->next_idx = 0;
}

int64_t sched_target(const sched_t *s, uint64_t idx)
{
    return s->start_ns + (int64_t)idx * s->period_ns;
}

sched_action_t sched_on_wake(sched_t *s, int64_t now, uint64_t *run_idx, int64_t *lateness,
                             uint64_t *skipped)
{
    *skipped = 0;
    if (s->next_idx >= s->end_idx)
        return SCHED_END;
    if (now >= s->end_ns) {
        /* 終了時刻前に予定され、実行されなかった周期はスキップとして数える */
        *skipped = count_before_end(s, s->next_idx, UINT64_MAX);
        s->next_idx = s->end_idx;
        return SCHED_END;
    }
    if (now < sched_target(s, s->next_idx))
        return SCHED_WAIT;

    /* nowを含む周期（予定時刻がnow以下の最後の周期） */
    uint64_t cur = (uint64_t)((now - s->start_ns) / s->period_ns);
    *skipped = cur - s->next_idx;
    *run_idx = cur;
    *lateness = now - sched_target(s, cur);
    s->next_idx = cur + 1;
    return SCHED_RUN;
}

uint64_t sched_after_burst(sched_t *s, int64_t now)
{
    /* 予定時刻がnowより前（過ぎた）周期を飛ばす。ちょうどnowの周期は未到来と同じ扱いで実行する。 */
    uint64_t next = first_idx_at_or_after(s, now);
    if (next <= s->next_idx)
        return 0;
    uint64_t skipped = count_before_end(s, s->next_idx, next);
    s->next_idx = next;
    return skipped;
}
