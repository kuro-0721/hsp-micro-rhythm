/*
 * game_loop.c —— 固定帧循环 + 帧间隔统计
 *
 * 对应 docs/02 §3 的 game_loop 模块与 §4.1 的 loop_stats()。
 *
 * 帧结构（每帧一次）：
 *   1) CPU 绘制：清屏 → 轨道分隔线 → 判定线 → 音符（全部写进背缓冲，不提交）
 *   2) 提交：fb_present()（整屏单次提交 + 等 tx_complete）
 *   3) 补足到 FRAME_MS：若本帧耗时不足 33ms，用 rt_thread_delay 补满，形成固定节拍
 *
 * 【临时】音符密度压力测试：
 *   M2 留下的问题是「整屏刷 p95 31ms 已达标，但 CPU 填充占 8ms，30fps 下只剩 4ms 余量，
 *   音符多了会不会掉帧没人测过」。所以本文件在启动后先自动跑一轮密度扫描：
 *     每轨 0 → 5 → 10 → 20 → 30 → 50 个音符，每档 200 帧，逐档打印帧统计。
 *   用数据决定「要不要立刻做脏区更新」，而不是靠猜。
 *   扫描结束后进入正常模式（每轨 8 个音符），压力测试代码在 M3 收口时删除。
 */
#include "app.h"
#include <string.h>
#include <stdlib.h>

#define FRAME_MS            33u     /* 目标帧间隔：30fps */
#define MISS_TOLERANCE      5u      /* 超过 33+5ms 记一次超时帧 */
#define RING_LEN            256u    /* 统计环形缓冲 */

#define LANE_COUNT          4
#define NOTE_H              12
#define NOTE_MARGIN         4
#define JUDGE_LINE_OFFSET   70      /* 判定线距屏幕底部的高度 */
#define NORMAL_NOTES        8       /* 正常模式下每轨音符数 */

/* 压力测试参数（M3 收口时连同 bench_* 一起删除） */
static const int s_bench_density[] = { 0, 5, 10, 20, 30, 50 };
#define BENCH_STEPS         ((int)(sizeof(s_bench_density) / sizeof(s_bench_density[0])))
#define BENCH_FRAMES        200u

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
static uint32_t s_frames;

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

static void stats_reset(void)
{
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    memset(&s_st, 0, sizeof(s_st));
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

/* ---------------- 绘制一帧 ---------------- */

static void render_scene(uint32_t frame, int notes_per_lane)
{
    int w = fb_width();
    int h = fb_height();
    int lane_w = w / LANE_COUNT;
    int judge_y = h - JUDGE_LINE_OFFSET;

    fb_clear(COL_BG);

    /* 轨道分隔线 */
    for (int i = 1; i < LANE_COUNT; i++)
        gfx_vline(i * lane_w, 0, h - 1, COL_LANE);

    /* 判定线（两像素高，便于肉眼定位） */
    gfx_hline(0, w - 1, judge_y, COL_JUDGE);
    gfx_hline(0, w - 1, judge_y + 1, COL_JUDGE);

    if (notes_per_lane <= 0)
        return;

    /* 音符：同一轨内等间隔排队，整体随时间向下滚动 */
    int period = h + NOTE_H;                    /* 滚动周期 */
    int gap = period / notes_per_lane;          /* 同轨相邻音符间隔 */
    int scroll = (int)((frame * 6u) % (uint32_t)period);
    int note_w = lane_w - NOTE_MARGIN * 2;

    if (note_w < 2)
        note_w = 2;

    for (int lane = 0; lane < LANE_COUNT; lane++)
    {
        int x0 = lane * lane_w + NOTE_MARGIN;
        int x1 = x0 + note_w - 1;

        for (int i = 0; i < notes_per_lane; i++)
        {
            int y = (i * gap + scroll) % period - NOTE_H;

            if (y + NOTE_H <= 0 || y >= h)
                continue;
            gfx_fill_rect(x0, y, x1, y + NOTE_H - 1, COL_NOTE);
        }
    }
}

/* ---------------- 帧循环 ---------------- */

static void bench_report(int step, int notes)
{
    struct loop_stat st;

    loop_stats(&st);
    rt_kprintf("[BENCH] notes/lane=%d frames=%u avg=%ums p95=%ums max=%ums "
               "draw=%ums flush=%ums miss=%u\n",
               notes, (unsigned)st.frames, (unsigned)st.avg_ms, (unsigned)st.p95_ms,
               (unsigned)st.max_ms, (unsigned)st.avg_draw_ms, (unsigned)st.avg_flush_ms,
               (unsigned)st.miss);
    (void)step;
}

static void game_thread(void *param)
{
    uint32_t frame = 0;
    int bench_step = 0;
    uint32_t bench_frame = 0;
    int notes = s_bench_density[0];

    rt_kprintf("[LOOP] start, frame=%ums, bench %d steps x %u frames\n",
               (unsigned)FRAME_MS, BENCH_STEPS, (unsigned)BENCH_FRAMES);

    for (;;)
    {
        uint32_t t0 = rt_tick_get_millisecond();

        render_scene(frame, notes);

        uint32_t t1 = rt_tick_get_millisecond();
        fb_present();
        uint32_t t2 = rt_tick_get_millisecond();

        stats_add(t1 - t0, t2 - t1);
        frame++;
        s_frames = frame;

        /* 临时压力测试：逐档统计并打印 */
        if (bench_step >= 0 && bench_step < BENCH_STEPS)
        {
            if (++bench_frame >= BENCH_FRAMES)
            {
                bench_report(bench_step, notes);
                stats_reset();
                bench_frame = 0;

                if (bench_step + 1 >= BENCH_STEPS)
                {
                    bench_step = -1;
                    notes = NORMAL_NOTES;
                    rt_kprintf("[BENCH] done -> normal mode (%d notes/lane)\n",
                               notes);
                }
                else
                {
                    bench_step++;
                    notes = s_bench_density[bench_step];
                }
            }
        }

        /* 固定节拍：把本帧剩余时间补满 */
        uint32_t used = t2 - t0;
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

    /* 触摸输入（REQ-003 / REQ-104） */
    if (touch_init() != 0)
        rt_kprintf("[APP] WARN: touch init failed, judge will get no input\n");

    /* 临时桥接：第 3 步 scene 接管输入后删除 input_bridge.c */
    input_bridge_start();

    return game_loop_start();
}
