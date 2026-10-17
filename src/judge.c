/*
 * judge.c —— 判定引擎：三档判定 + Combo/Acc 统计 + 板上自检
 *
 * 对应 docs/02 §3 的 judge 模块与 §4.1/§4.2 的接口与数据结构。
 *
 * 判定规则（REQ-003）：
 *   |t_click - note.hit_ms| <= 60ms  → PERFECT
 *   |t_click - note.hit_ms| <= 120ms → GOOD
 *   超过 120ms 仍未判定              → MISS（由 judge_tick 在时间推进时产生）
 *
 * 两条容易被忽略、但决定手感是否「讲道理」的规则：
 *   1. 点击时若该轨最近的待判音符距离 > 120ms，**不消耗音符也不判 MISS**，
 *      返回 JUDGE_NONE。否则「提前乱点」会把后面的音符白白吃掉。
 *   2. 一次点击最多消耗一个音符（REQ-003）。
 *
 * Acc 口径（docs/01 §1.2「按判定档位加权的命中率」）：
 *   acc = (perfect*100 + good*50) / judged，存为百分比×100 的整数（acc_x100），
 *   例如 1 PERFECT + 1 GOOD → acc_x100 = 7500 表示 75.00%。
 *   MISS 计入分母、得分为 0。权重是常量，若产品上要调整只需改这里与文档。
 *
 * 分数：PERFECT +100、GOOD +50（P0 不设 Combo 倍数，避免规则复杂化）。
 */
#include "app.h"
#include <string.h>

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define SCORE_PERFECT   100u
#define SCORE_GOOD      50u

static struct note s_notes[JUDGE_MAX_NOTES];
static uint32_t s_note_count;
static struct judge_stat s_stat;

static void recalc_acc(void)
{
    if (s_stat.judged == 0u)
    {
        s_stat.acc_x100 = 0u;
        return;
    }

    /* (perfect*100 + good*50) / judged，再 ×100 保留两位小数 */
    uint64_t num = (uint64_t)s_stat.perfect * 10000ull
                 + (uint64_t)s_stat.good * 5000ull;
    s_stat.acc_x100 = (uint32_t)(num / (uint64_t)s_stat.judged);
}

int judge_init(void)
{
    memset(s_notes, 0, sizeof(s_notes));
    memset(&s_stat, 0, sizeof(s_stat));
    s_note_count = 0;
    return 0;
}

int judge_load(const struct note *notes, uint32_t n)
{
    if (!notes || n > JUDGE_MAX_NOTES)
    {
        rt_kprintf("[JUDGE] ERROR: load %u notes failed (max %u)\n",
                   (unsigned)n, (unsigned)JUDGE_MAX_NOTES);
        return -1;
    }

    memset(s_notes, 0, sizeof(s_notes));
    memcpy(s_notes, notes, n * sizeof(struct note));
    for (uint32_t i = 0; i < n; i++)
        s_notes[i].state = 0;
    s_note_count = n;

    memset(&s_stat, 0, sizeof(s_stat));
    return 0;
}

void judge_reset(void)
{
    for (uint32_t i = 0; i < s_note_count; i++)
        s_notes[i].state = 0;
    memset(&s_stat, 0, sizeof(s_stat));
}

enum judge_result judge_touch(uint8_t lane, uint32_t t_ms)
{
    int best = -1;
    uint32_t best_dt = 0xFFFFFFFFu;

    if (lane >= 4u)
        return JUDGE_NONE;

    /* 找该轨上「待判定」且离点击时刻最近的音符 */
    for (uint32_t i = 0; i < s_note_count; i++)
    {
        const struct note *n = &s_notes[i];
        uint32_t dt;

        if (n->state || n->lane != lane)
            continue;

        dt = (t_ms > n->hit_ms) ? (t_ms - n->hit_ms) : (n->hit_ms - t_ms);
        if (dt < best_dt)
        {
            best_dt = dt;
            best = (int)i;
        }
    }

    if (best < 0)
        return JUDGE_NONE;

    /* 超出 Good 窗口：不命中、不消耗，交给 judge_tick 判 MISS */
    if (best_dt > JUDGE_GOOD_MS)
        return JUDGE_NONE;

    struct note *hit = &s_notes[best];
    enum judge_result r = (best_dt <= JUDGE_PERFECT_MS) ? JUDGE_PERFECT : JUDGE_GOOD;

    hit->state = 1;
    s_stat.judged++;
    s_stat.combo++;
    if (s_stat.combo > s_stat.max_combo)
        s_stat.max_combo = s_stat.combo;

    if (r == JUDGE_PERFECT)
    {
        s_stat.perfect++;
        s_stat.score += SCORE_PERFECT;
    }
    else
    {
        s_stat.good++;
        s_stat.score += SCORE_GOOD;
    }

    recalc_acc();
    return r;
}

uint32_t judge_tick(uint32_t now_ms)
{
    uint32_t new_miss = 0;

    for (uint32_t i = 0; i < s_note_count; i++)
    {
        struct note *n = &s_notes[i];

        if (n->state)
            continue;

        /* 超过 Good 窗口仍未判定 → MISS */
        if (now_ms > n->hit_ms + JUDGE_GOOD_MS)
        {
            n->state = 1;
            s_stat.judged++;
            s_stat.miss++;
            s_stat.combo = 0;
            new_miss++;
        }
    }

    if (new_miss)
        recalc_acc();

    return new_miss;
}

void judge_stats(struct judge_stat *out)
{
    if (!out)
        return;
    *out = s_stat;
}

const char *judge_result_name(enum judge_result r)
{
    switch (r)
    {
    case JUDGE_PERFECT: return "PERFECT";
    case JUDGE_GOOD:    return "GOOD";
    case JUDGE_MISS:    return "MISS";
    default:            return "NONE";
    }
}

/* ---------------- 板上自检（REQ-007 的一部分） ---------------- */

static int selftest_one(uint32_t hit_ms, uint32_t click_ms, uint8_t lane,
                        enum judge_result expect, const char *tag)
{
    struct note n;
    enum judge_result got;

    judge_init();
    memset(&n, 0, sizeof(n));
    n.hit_ms = hit_ms;
    n.lane = lane;

    if (judge_load(&n, 1) != 0)
    {
        rt_kprintf("[SELFTEST] judge %s: FAIL (load)\n", tag);
        return 1;
    }

    got = judge_touch(lane, click_ms);
    if (got != expect)
    {
        rt_kprintf("[SELFTEST] judge %s: FAIL (expect %s, got %s)\n",
                   tag, judge_result_name(expect), judge_result_name(got));
        return 1;
    }
    return 0;
}

int judge_selftest(void)
{
    int fail = 0;
    struct judge_stat st;

    /* 1) 判定窗口边界（REQ-003：Perfect ≤±60ms、Good ≤±120ms） */
    fail += selftest_one(1000,  940, 0, JUDGE_PERFECT, "boundary -60ms");
    fail += selftest_one(1000, 1060, 0, JUDGE_PERFECT, "boundary +60ms");
    fail += selftest_one(1000,  941, 0, JUDGE_PERFECT, "boundary -59ms");
    fail += selftest_one(1000, 1061, 0, JUDGE_GOOD,    "boundary +61ms");
    fail += selftest_one(1000,  880, 0, JUDGE_GOOD,    "boundary -120ms");
    fail += selftest_one(1000, 1120, 0, JUDGE_GOOD,    "boundary +120ms");
    fail += selftest_one(1000, 1121, 0, JUDGE_NONE,    "over +121ms");
    fail += selftest_one(1000,  879, 0, JUDGE_NONE,    "over -121ms");

    /* 2) 一次点击只消耗一个音符（REQ-003） */
    {
        struct note two[2];
        judge_init();
        memset(two, 0, sizeof(two));
        two[0].hit_ms = 1000; two[0].lane = 0;
        two[1].hit_ms = 1020; two[1].lane = 0;
        judge_load(two, 2);

        if (judge_touch(0, 1000) != JUDGE_PERFECT)
        {
            rt_kprintf("[SELFTEST] judge one-tap: FAIL (first tap)\n");
            fail++;
        }
        judge_stats(&st);
        if (st.judged != 1u)
        {
            rt_kprintf("[SELFTEST] judge one-tap: FAIL (judged=%u, expect 1)\n",
                       (unsigned)st.judged);
            fail++;
        }
        /* 再点一次应命中第二个音符，而不是重复消耗第一个 */
        if (judge_touch(0, 1030) != JUDGE_PERFECT)
        {
            rt_kprintf("[SELFTEST] judge one-tap: FAIL (second tap)\n");
            fail++;
        }
        judge_stats(&st);
        if (st.judged != 2u)
        {
            rt_kprintf("[SELFTEST] judge one-tap: FAIL (judged=%u, expect 2)\n",
                       (unsigned)st.judged);
            fail++;
        }
    }

    /* 3) Combo：命中累加，MISS 归零 */
    {
        struct note three[3];
        judge_init();
        memset(three, 0, sizeof(three));
        three[0].hit_ms = 1000; three[0].lane = 0;
        three[1].hit_ms = 1100; three[1].lane = 1;
        three[2].hit_ms = 2000; three[2].lane = 2;
        judge_load(three, 3);

        judge_touch(0, 1000);
        judge_touch(1, 1100);
        judge_stats(&st);
        if (st.combo != 2u || st.max_combo != 2u)
        {
            rt_kprintf("[SELFTEST] judge combo: FAIL (combo=%u max=%u, expect 2/2)\n",
                       (unsigned)st.combo, (unsigned)st.max_combo);
            fail++;
        }
        /* 第三个音符超时 → MISS → combo 归零 */
        if (judge_tick(2200) != 1u)
        {
            rt_kprintf("[SELFTEST] judge combo: FAIL (tick miss count)\n");
            fail++;
        }
        judge_stats(&st);
        if (st.combo != 0u || st.miss != 1u || st.max_combo != 2u)
        {
            rt_kprintf("[SELFTEST] judge combo: FAIL (combo=%u miss=%u max=%u)\n",
                       (unsigned)st.combo, (unsigned)st.miss, (unsigned)st.max_combo);
            fail++;
        }
    }

    /* 4) Acc 与分数：1 PERFECT + 1 GOOD → acc 75.00%、score 150 */
    {
        struct note two[2];
        judge_init();
        memset(two, 0, sizeof(two));
        two[0].hit_ms = 1000; two[0].lane = 0;
        two[1].hit_ms = 2000; two[1].lane = 1;
        judge_load(two, 2);

        judge_touch(0, 1000);      /* PERFECT */
        judge_touch(1, 2100);      /* GOOD（+100ms） */
        judge_stats(&st);
        if (st.acc_x100 != 7500u)
        {
            rt_kprintf("[SELFTEST] judge acc: FAIL (acc_x100=%u, expect 7500)\n",
                       (unsigned)st.acc_x100);
            fail++;
        }
        if (st.score != 150u)
        {
            rt_kprintf("[SELFTEST] judge score: FAIL (score=%u, expect 150)\n",
                       (unsigned)st.score);
            fail++;
        }
    }

    /* 5) 不同轨互不干扰 */
    {
        struct note n;
        judge_init();
        memset(&n, 0, sizeof(n));
        n.hit_ms = 1000;
        n.lane = 2;
        judge_load(&n, 1);

        if (judge_touch(1, 1000) != JUDGE_NONE)
        {
            rt_kprintf("[SELFTEST] judge lane: FAIL (lane 1 should not hit lane 2 note)\n");
            fail++;
        }
        if (judge_touch(2, 1000) != JUDGE_PERFECT)
        {
            rt_kprintf("[SELFTEST] judge lane: FAIL (lane 2 should hit)\n");
            fail++;
        }
    }

    /* 自检结束：清空，避免把测试音符/统计带进真实对局 */
    judge_init();

    if (fail == 0)
        rt_kprintf("[SELFTEST] judge: PASS (window/combo/acc/score/lane)\n");
    else
        rt_kprintf("[SELFTEST] judge: FAIL (%d case(s))\n", fail);
    return fail;
}

/*
 * 批量判定测试（TC-003：以已知时刻输入 ≥50 次，判定结果与理论一致）。
 *
 * 用程序生成 count 个音符（间隔 500ms、4 轨轮流）与 count 次点击，
 * 点击偏移在 -119..+119ms 之间扫过，期望值由 |offset| 与窗口常量直接求出，
 * 逐条比对并把不一致的打印出来。测试结束后清空状态，不污染真实对局。
 * 返回失败的用例数（0 = 全对）。
 */
int judge_bulk_test(uint32_t count)
{
    static struct note notes[JUDGE_MAX_NOTES];
    uint32_t bad = 0;
    struct judge_stat st;

    if (count == 0u || count > JUDGE_MAX_NOTES)
        count = 50u;

    judge_init();
    memset(notes, 0, sizeof(notes));
    for (uint32_t i = 0; i < count; i++)
    {
        notes[i].hit_ms = 1000u + i * 500u;
        notes[i].lane = (uint8_t)(i % 4u);
    }
    judge_load(notes, count);

    rt_kprintf("[BULK] judge: %u notes, click offset sweep -119..+119ms\n",
               (unsigned)count);

    for (uint32_t i = 0; i < count; i++)
    {
        int32_t d = (int32_t)((i * 37u) % 239u) - 119;   /* -119 .. +119 */
        uint32_t click = (uint32_t)((int32_t)notes[i].hit_ms + d);
        uint32_t ad = (d < 0) ? (uint32_t)(-d) : (uint32_t)d;
        enum judge_result expect = (ad <= JUDGE_PERFECT_MS) ? JUDGE_PERFECT : JUDGE_GOOD;
        enum judge_result got = judge_touch(notes[i].lane, click);

        if (got != expect)
        {
            if (bad < 5u)
                rt_kprintf("[BULK] FAIL #%u offset=%dms expect=%s got=%s\n",
                           (unsigned)i, (int)d, judge_result_name(expect),
                           judge_result_name(got));
            bad++;
        }
    }

    judge_stats(&st);
    rt_kprintf("[BULK] judge: %s (%u/%u ok, P=%u G=%u M=%u score=%u acc=%u.%02u%%)\n",
               bad ? "FAIL" : "PASS",
               (unsigned)(count - bad), (unsigned)count,
               (unsigned)st.perfect, (unsigned)st.good, (unsigned)st.miss,
               (unsigned)st.score,
               (unsigned)(st.acc_x100 / 100u), (unsigned)(st.acc_x100 % 100u));

    judge_init();   /* 不留测试状态 */
    return (int)bad;
}
