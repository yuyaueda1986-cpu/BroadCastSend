/* スケジューラーの単体テスト（時刻は仮想値を与える）。単位はμs相当の値をnsとして扱う。 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "cycle_sched.h"

static int failures = 0;

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        failures++; \
    } \
} while (0)

static void test_on_time(void)
{
    sched_t s;
    sched_init(&s, 0, 100, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    CHECK_EQ(sched_on_wake(&s, 0, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 0); CHECK_EQ(late, 0); CHECK_EQ(sk, 0);
    CHECK_EQ(sched_after_burst(&s, 10), 0);
    CHECK_EQ(sched_next_target(&s), 100);
    /* 予定時刻前の起床は待機 */
    CHECK_EQ(sched_on_wake(&s, 99, &idx, &late, &sk), SCHED_WAIT);
    CHECK_EQ(sched_on_wake(&s, 100, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 1); CHECK_EQ(late, 0); CHECK_EQ(sk, 0);
}

/* 1周期未満の遅れはスキップせず実行し、遅れを記録する */
static void test_minor_delay(void)
{
    sched_t s;
    sched_init(&s, 0, 100, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    sched_on_wake(&s, 0, &idx, &late, &sk);
    sched_after_burst(&s, 5);
    CHECK_EQ(sched_on_wake(&s, 199, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 1); CHECK_EQ(late, 99); CHECK_EQ(sk, 0);
}

/* PLAN.mdの例：起床350で100・200をスキップし300を実行、450終了で400もスキップ、次は500 */
static void test_plan_example(void)
{
    sched_t s;
    sched_init(&s, 0, 100, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    sched_on_wake(&s, 0, &idx, &late, &sk);
    CHECK_EQ(sched_after_burst(&s, 20), 0);
    CHECK_EQ(sched_on_wake(&s, 350, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 3); CHECK_EQ(late, 50); CHECK_EQ(sk, 2);
    CHECK_EQ(sched_after_burst(&s, 450), 1);
    CHECK_EQ(sched_next_target(&s), 500);
}

/* バーストがちょうど次の予定時刻に終わった場合はスキップしない */
static void test_burst_ends_exactly_on_target(void)
{
    sched_t s;
    sched_init(&s, 0, 100, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    sched_on_wake(&s, 0, &idx, &late, &sk);
    CHECK_EQ(sched_after_burst(&s, 100), 0);
    CHECK_EQ(sched_next_target(&s), 100);
    CHECK_EQ(sched_after_burst(&s, 101), 1);
    CHECK_EQ(sched_next_target(&s), 200);
}

/* 開始時刻が0以外でも同じ計算になる */
static void test_nonzero_start(void)
{
    sched_t s;
    sched_init(&s, 1000000, 100, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    CHECK_EQ(sched_on_wake(&s, 1000000, &idx, &late, &sk), SCHED_RUN);
    sched_after_burst(&s, 1000010);
    CHECK_EQ(sched_on_wake(&s, 1000350, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 3); CHECK_EQ(late, 50); CHECK_EQ(sk, 2);
}

/* 終了時刻以降の予定周期はスキップに数えない */
static void test_end_time(void)
{
    sched_t s;
    sched_init(&s, 0, 100, 1000); /* 周期0〜9の10周期 */
    CHECK_EQ(s.end_idx, 10);
    uint64_t idx, sk;
    int64_t late;
    sched_on_wake(&s, 0, &idx, &late, &sk);
    sched_after_burst(&s, 10);
    /* 850で起床：100〜700の7周期をスキップし800を実行 */
    CHECK_EQ(sched_on_wake(&s, 850, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, 8); CHECK_EQ(sk, 7);
    /* バーストが5000まで続いた：900だけがスキップ（1000以降は数えない） */
    CHECK_EQ(sched_after_burst(&s, 5000), 1);
    CHECK_EQ(sched_on_wake(&s, 5000, &idx, &late, &sk), SCHED_END);
    CHECK_EQ(sk, 0);
}

/* 終了時刻を過ぎて起床した場合、終了前の未実行周期だけを数える */
static void test_wake_after_end(void)
{
    sched_t s;
    sched_init(&s, 0, 100, 1000);
    uint64_t idx, sk;
    int64_t late;
    sched_on_wake(&s, 0, &idx, &late, &sk);
    sched_after_burst(&s, 10);
    CHECK_EQ(sched_on_wake(&s, 1500, &idx, &late, &sk), SCHED_END);
    CHECK_EQ(sk, 9);
    CHECK_EQ(sched_on_wake(&s, 1600, &idx, &late, &sk), SCHED_END);
    CHECK_EQ(sk, 0);
}

/* 終了時刻が周期の途中にある場合 */
static void test_end_not_aligned(void)
{
    sched_t s;
    sched_init(&s, 0, 100, 250); /* 0,100,200の3周期 */
    CHECK_EQ(s.end_idx, 3);
    uint64_t idx, sk;
    int64_t late;
    int runs = 0;
    int64_t now = 0;
    for (;;) {
        now = sched_next_target(&s);
        sched_action_t a = sched_on_wake(&s, now, &idx, &late, &sk);
        if (a == SCHED_END)
            break;
        CHECK_EQ(a, SCHED_RUN);
        runs++;
        sched_after_burst(&s, now + 1);
    }
    CHECK_EQ(runs, 3);
}

/* 実運用規模の周期番号でもオーバーフローしない（100μs周期で1年） */
static void test_large_index(void)
{
    sched_t s;
    int64_t start = 123456789;
    sched_init(&s, start, 100000, SCHED_NO_END);
    uint64_t idx, sk;
    int64_t late;
    int64_t year_ns = 365LL * 24 * 3600 * 1000000000LL;
    sched_on_wake(&s, start, &idx, &late, &sk);
    sched_after_burst(&s, start + 10);
    CHECK_EQ(sched_on_wake(&s, start + year_ns + 5, &idx, &late, &sk), SCHED_RUN);
    CHECK_EQ(idx, year_ns / 100000);
    CHECK_EQ(late, 5);
    CHECK_EQ(sk, year_ns / 100000 - 1);
}

int main(void)
{
    test_on_time();
    test_minor_delay();
    test_plan_example();
    test_burst_ends_exactly_on_target();
    test_nonzero_start();
    test_end_time();
    test_wake_after_end();
    test_end_not_aligned();
    test_large_index();
    if (failures) {
        fprintf(stderr, "test_sched: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sched: OK\n");
    return 0;
}
