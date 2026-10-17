/*
 * input_bridge.c —— 【临时】触摸事件 → 判定引擎的桥接
 *
 * M3 第 2 步还没有 chart / scene（第 3 步才做），但没有场景就没人消费触摸事件，
 * 也就无法验证「触摸链路 + 判定接口」是否连通。本文件只做这一件事：
 *   轮询 touch_pop_event() → 把 x 坐标映射成轨号 → 调 judge_touch() → 打印结果
 *
 * 第 3 步 scene.c 接管输入后，本文件删除。
 *
 * 注意：本模块**不参与渲染**，也不在帧循环线程里跑，只做输入搬运，
 * 因此不会影响 p95 已压在 33ms 线上的帧循环。
 */
#include "app.h"

#define POLL_INTERVAL_MS    5u

static void bridge_thread(void *param)
{
    struct touch_ev ev;
    uint32_t n = 0;
    int w = fb_width();
    int lane_w = (w > 0) ? (w / 4) : 1;

    (void)param;

    if (lane_w <= 0)
        lane_w = 1;

    for (;;)
    {
        if (touch_pop_event(&ev) == 0)
        {
            int lane = (int)ev.x / lane_w;

            if (lane < 0)
                lane = 0;
            if (lane > 3)
                lane = 3;

            n++;
            rt_kprintf("[TP] #%u DOWN (%u,%u) t=%ums lane=%d judge=%s\n",
                       (unsigned)n, (unsigned)ev.x, (unsigned)ev.y,
                       (unsigned)ev.t_ms, lane,
                       judge_result_name(judge_touch((uint8_t)lane, ev.t_ms)));
        }

        rt_thread_delay(rt_tick_from_millisecond(POLL_INTERVAL_MS));
    }
}

int input_bridge_start(void)
{
    rt_thread_t tid = rt_thread_create("inbr", bridge_thread, RT_NULL, 2048, 19, 10);

    if (!tid)
    {
        rt_kprintf("[INPUT] ERROR: create bridge thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    rt_kprintf("[INPUT] bridge ready (touch -> judge, print only)\n");
    return 0;
}
