/*
 * autoplay.c —— 自动演奏（测试用）
 *
 * 为什么需要它：
 *   REQ-101 的字面判据是「**连续演奏** 30s，帧间隔 P95 ≤34ms」，
 *   REQ-103 是「连续演奏 10min 不死机，每 60s 记 free heap」。
 *   但人不可能手打 30 秒不失误、更不可能手打 10 分钟，
 *   所以 docs/04 里这两项一直是「连续运行」而不是「连续演奏」——
 *   2026-10-31 已如实登记该口径局限。
 *   本模块按谱面自动注入点击，让帧循环真的处在「演奏 + BGM + 命中音效」的负载下。
 *
 * 它怎么注入：
 *   不是直接调 judge_touch()，而是走 touch_inject() 把事件塞进触摸事件队列，
 *   下游路径（scene_update 的事件分派 → chart_ev 换算 → judge_touch → sfx_trigger）
 *   与真实触摸**完全一致**。区别只有两点，报告里必须写明：
 *     1) 事件是软件生成的，不经过 FT6146 中断与 I2C 读点；
 *     2) 因此本模式测不到「触摸中断→判定」的真实延迟（TC-104 不适用）。
 *
 * 关掉它：
 *   把 AUTOPLAY_ENABLE 改成 0 即整体停用（线程不会创建）。
 *   正式演示前必须置 0。
 */
#include "app.h"

#define AUTOPLAY_ENABLE     0
#define AUTOPLAY_LANES      4       /* 与 scene.c 的 LANE_COUNT 一致 */
#define AUTOPLAY_PERIOD_MS  5       /* 轮询周期：远小于 ±60ms 的 Perfect 窗口 */
#define AUTOPLAY_LEAD_MS    5       /* 谱面时间追平判定阈值后就打 */
#define AUTOPLAY_PRIO       26      /* 低于 game(20) 与 sfx(24)，不去挤帧循环 */

#if AUTOPLAY_ENABLE

static int s_started;

static void autoplay_thread(void *param)
{
    const struct note *notes;
    uint32_t n;
    uint32_t idx = 0;
    uint32_t last_title_tap = 0;
    enum scene_id prev = SCENE_TITLE;
    uint32_t hits = 0;

    (void)param;

    while (1)
    {
        enum scene_id sc = scene_current();
        uint32_t now = rt_tick_get_millisecond();

        if (sc == SCENE_PLAYING)
        {
            if (prev != SCENE_PLAYING)
            {
                /* 新一局：从头开始按谱面打 */
                idx = 0;
                hits = 0;
                rt_kprintf("[AUTO] play round start, notes=%u\n",
                           (unsigned)chart_note_count());
            }

            notes = chart_notes();
            n = chart_note_count();
            if (notes && n > 0)
            {
                uint32_t cm = scene_chart_ms();
                int w = fb_width();
                int lane_w = (w > 0) ? (w / AUTOPLAY_LANES) : 1;

                if (lane_w < 1)
                    lane_w = 1;

                while (idx < n && notes[idx].hit_ms <= (cm + AUTOPLAY_LEAD_MS))
                {
                    int lane = (int)notes[idx].lane;

                    if (lane < 0)
                        lane = 0;
                    if (lane >= AUTOPLAY_LANES)
                        lane = AUTOPLAY_LANES - 1;

                    touch_inject(now,
                                 (uint16_t)(lane * lane_w + lane_w / 2),
                                 (uint16_t)(fb_height() / 2));
                    idx++;
                    hits++;
                }
            }
        }
        else if (sc == SCENE_TITLE)
        {
            /*
             * 自动开局：点屏幕上部（h/3），避开 TITLE 底部的 SFX 开关按钮，
             * 否则会把音效关掉、后面的命中就不响了。
             */
            if ((uint32_t)(now - last_title_tap) > 1200u)
            {
                last_title_tap = now;
                touch_inject(now, (uint16_t)(fb_width() / 2),
                             (uint16_t)(fb_height() / 3));
            }
        }

        if (prev == SCENE_PLAYING && sc != SCENE_PLAYING)
            rt_kprintf("[AUTO] play round end, hits=%u\n", (unsigned)hits);

        prev = sc;
        rt_thread_mdelay(AUTOPLAY_PERIOD_MS);
    }
}

int autoplay_start(void)
{
    rt_thread_t tid;

    if (s_started)
        return 0;

    tid = rt_thread_create("autoplay", autoplay_thread, RT_NULL, 2048,
                           AUTOPLAY_PRIO, 10);
    if (!tid)
    {
        rt_kprintf("[AUTO] ERROR: create thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    s_started = 1;
    rt_kprintf("[AUTO] enabled: taps are injected by software, not by touch IRQ\n");
    return 0;
}

#else

int autoplay_start(void)
{
    return 0;
}

#endif /* AUTOPLAY_ENABLE */
