/*
 * input_bridge.c —— 【临时】触摸事件 → 判定引擎的桥接
 *
 * M3 第 2 步还没有 chart / scene（第 3 步才做），但没有场景就没人消费触摸事件，
 * 也就无法验证「触摸链路 + 判定接口」是否连通。本文件只做这一件事：
 *   轮询 touch_pop_event() → 把 x 坐标映射成轨号 → 调 judge_touch() → 打印结果
 *
 * 第 3 步 scene.c 接管输入后，本文件删除。
 *
 * 注意：本模块**不参与渲染**，也不在帧循环线程里跑，只做输入搬运。
 * 实测：加入本模块后 p95 从 33ms 变为 34ms（REQ-101 已按 CR-04 放宽到 ≤34ms）。
 * 但该 1ms 变化**与本线程的优先级无关**（曾从 19 改为 25，数据纹丝不动），
 * 也不在触摸事件的数量（全程不碰屏仍为 34ms），具体原因未定为 R-10。
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
    /*
     * 优先级 25：低于帧循环线程 game（20），因为 RT-Thread 里数字越小优先级越高。
     * 本线程只是 5ms 轮询的搬运工，没有理由抢占渲染。
     *
     * 【已被实测证伪的说法，保留记录】最初改这个优先级时以为它是 p95 从 33ms 变 34ms 的原因，
     * 但对比 evidence/M3/judge_run2（prio 19，p95 34ms）与 judge_run3（prio 25，p95 34ms）
     * 可以看到两轮数据完全一致——优先级不是原因。保留 25 只是因为它本身合理，不是为了修 p95。
     */
    rt_thread_t tid = rt_thread_create("inbr", bridge_thread, RT_NULL, 2048, 25, 10);

    if (!tid)
    {
        rt_kprintf("[INPUT] ERROR: create bridge thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    rt_kprintf("[INPUT] bridge ready (touch -> judge, prio 25 < game 20)\n");
    return 0;
}
