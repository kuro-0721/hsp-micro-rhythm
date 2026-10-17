/*
 * game_loop.c —— 固定帧循环 + 帧间隔统计
 *
 * 对应 docs/02 §3 的 game_loop 模块与 §4.1 的 loop_stats()。
 *
 * 帧结构（每帧一次，固定 33ms 节拍）：
 *   1) 逻辑：scene_update()——出队触摸、推进判定、状态切换
 *   2) 绘制：scene_render()——只写背缓冲，不提交
 *   3) 提交：fb_present()（整屏单次提交 + 等 tx_complete）
 *   4) 补足到 FRAME_MS：若本帧耗时不足 33ms，用 rt_thread_delay 补满
 *
 * 【历史】M3 第 1~2 步曾在本文件里放了一段「音符密度压力测试」（6 档 × 200 帧），
 * 用来回答「200 个音符时 draw 会不会拖慢帧率」。数据已入库
 * （evidence/M3/bench_serial_2026-10-17.log）：draw 2→6ms、主体开销是等 VSYNC。
 * 第 3 步场景接管渲染后，那段临时代码已删除，需要复现时用 git 历史回退。
 */
#include "app.h"
#include <string.h>
#include <stdlib.h>

#define FRAME_MS            33u     /* 目标帧间隔：30fps */
#define MISS_TOLERANCE      5u      /* 超过 33+5ms 记一次超时帧 */
#define RING_LEN            256u    /* 统计环形缓冲 */
#define REPORT_EVERY_FRAMES 300u    /* 约 10s 打印一次帧统计 */

struct stats
{
    uint32_t n;
    uint32_t sum_total, sum_draw, sum_flush;
    uint32_t max_total;
    uint32_t miss;
    uint32_t ring[RING_LEN];
};

static struct stats s_st;
static struct rt_mutex s_lock;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t stats_p95(const struct stats *s)
{
    uint32_t sorted[RING_LEN];
    uint32_t n = (s->n < RING_LEN) ? s->n : RING_LEN;
    uint32_t idx;

    if (n == 0)
        return 0;
    memcpy(sorted, s->ring, n * sizeof(uint32_t));
    qsort(sorted, n, sizeof(uint32_t), cmp_u32);

    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

static void stats_add(uint32_t draw_ms, uint32_t flush_ms)
{
    uint32_t total = draw_ms + flush_ms;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_st.n < RING_LEN)
        s_st.ring[s_st.n] = total;
    else
        s_st.ring[(s_st.n) % RING_LEN] = total;
    s_st.sum_draw += draw_ms;
    s_st.sum_flush += flush_ms;
    s_st.sum_total += total;
    if (total > s_st.max_total)
        s_st.max_total = total;
    if (total > FRAME_MS + MISS_TOLERANCE)
        s_st.miss++;
    s_st.n++;
    rt_mutex_release(&s_lock);
}

void loop_stats(struct loop_stat *out)
{
    if (!out)
        return;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    out->frames = s_st.n;
    out->avg_ms = s_st.n ? (s_st.sum_total / s_st.n) : 0u;
    out->avg_draw_ms = s_st.n ? (s_st.sum_draw / s_st.n) : 0u;
    out->avg_flush_ms = s_st.n ? (s_st.sum_flush / s_st.n) : 0u;
    out->max_ms = s_st.max_total;
    out->p95_ms = stats_p95(&s_st);
    out->miss = s_st.miss;
    out->target_ms = FRAME_MS;
    rt_mutex_release(&s_lock);
}

/* ---------------- 帧统计打印 ---------------- */

static void stats_report(void)
{
    struct loop_stat st;

    loop_stats(&st);
    rt_kprintf("[LOOP] frames=%u avg=%ums p95=%ums max=%ums draw=%ums "
               "flush=%ums miss=%u\n",
               (unsigned)st.frames, (unsigned)st.avg_ms, (unsigned)st.p95_ms,
               (unsigned)st.max_ms, (unsigned)st.avg_draw_ms,
               (unsigned)st.avg_flush_ms, (unsigned)st.miss);
}

/* ---------------- 帧循环 ---------------- */

static void game_thread(void *param)
{
    uint32_t frame = 0;

    (void)param;

    rt_kprintf("[LOOP] start, frame=%ums\n", (unsigned)FRAME_MS);

    for (;;)
    {
        uint32_t t0 = rt_tick_get_millisecond();
        uint32_t t1, t2, used;

        /* 1) 逻辑：出队触摸、推进判定、状态切换 */
        scene_update(t0);

        /* 2) 绘制：只写背缓冲，不提交 */
        scene_render();

        t1 = rt_tick_get_millisecond();

        /* 3) 提交整屏并等 tx_complete */
        fb_present();

        t2 = rt_tick_get_millisecond();

        stats_add(t1 - t0, t2 - t1);
        frame++;

        /* 约 10s 打印一次帧统计，供现场观察与 REQ-103 长期运行验证 */
        if ((frame % REPORT_EVERY_FRAMES) == 0u)
            stats_report();

        /* 4) 固定节拍：把本帧剩余时间补满 */
        used = t2 - t0;
        if (used < FRAME_MS)
            rt_thread_delay(rt_tick_from_millisecond(FRAME_MS - used));
    }
}

int game_loop_start(void)
{
    rt_thread_t tid;

    if (!fb_backbuffer())
    {
        rt_kprintf("[LOOP] ERROR: framebuffer not ready, call fb_init() first\n");
        return -1;
    }

    rt_mutex_init(&s_lock, "loopst", RT_IPC_FLAG_FIFO);

    /* 栈 8KB：stats_p95() 在栈上排 256 个 uint32（1KB），留足余量 */
    tid = rt_thread_create("game", game_thread, RT_NULL, 8192, 20, 10);
    if (!tid)
    {
        rt_kprintf("[LOOP] ERROR: create game thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    return 0;
}

int app_start(void)
{
    int fail;

    if (fb_init() != 0)
        return -1;

    /* 判定引擎：先跑纯逻辑自检 + 批量判定测试
     * （REQ-007 的一部分；批量测试对应 TC-003 「以已知时刻输入 ≥50 次」） */
    judge_init();
    fail = judge_selftest();
    fail += judge_bulk_test(50u);
    rt_kprintf("[APP] judge selftest+bulk: %s\n", fail ? "FAIL" : "PASS");

    /*
     * REQ-006 冒烟：故意写错的谱面必须被拒（第 2 行轨号 9 越界）。
     * 必须在 chart_load_builtin() 之前做——chart_parse() 成功时会覆盖内部音符池，
     * 放在后面会把好谱面冲掉。正式的三节自检在第 4 步的 selftest.c 里汇总。
     */
    {
        int bad = chart_parse("# 冒烟：故意错误的谱面\n1500 9\n", -1);

        rt_kprintf("[CHART] bad chart -> %d (%s, expect -2)\n",
                   bad, (bad < 0) ? "REJECTED" : "NOT REJECTED");
        fail += (bad == -2) ? 0 : 1;
    }

    /* 谱面（REQ-006）：解析内置谱面并装入判定引擎 */
    if (chart_load_builtin() <= 0)
        fail++;

    /* 触摸输入（REQ-003 / REQ-104） */
    if (touch_init() != 0)
        rt_kprintf("[APP] WARN: touch init failed, player will get no input\n");

    /* 场景状态机（REQ-001）：上电进入 TITLE */
    scene_init();

    rt_kprintf("[APP] init done, errors=%d -> TITLE\n", fail);

    return game_loop_start();
}
