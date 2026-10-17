/*
 * selftest.c —— 启动自检（REQ-007 / TC-007）
 *
 * 对应 docs/02 §3 的 selftest 模块与 §4.1 的接口，判据见 docs/03 的 TC-007。
 *
 * 三节内容与执行时机（为什么不能一次性全部在启动时打印）：
 *   第 1 节 谱面解析（REQ-006）——启动时即可，不依赖运行期数据
 *   第 2 节 判定引擎（REQ-003）——启动时即可；含判定延迟直方图
 *   第 3 节 帧率统计（REQ-101）——**必须等跑够样本**（SELFTEST_FRAME_WINDOW
 *            ≈ 150 帧 ≈ 5s），否则 p95 无意义。由帧循环回调一次。
 *
 * 【顺序很关键】第 2 节必须排在谱面之前，因为 judge_bulk_test() 结尾会调
 * judge_init() 清空音符池；如果先加载谱面再跑批量测试，演奏时就没有音符了。
 * 反过来，谱面节里的「错误谱面测试」会把 chart 的内部状态置为无效，所以它
 * 必须在加载内置谱面之前做。
 *
 * TC-007 的「注入错误谱面时对应项显示 FAIL」用编译开关演示，见下方
 * SELFTEST_INJECT_BAD_CHART。默认 0，正常启动。
 */
#include "app.h"
#include <string.h>

/*
 * 演示开关：改成 1 重新编译烧录后，第 1 节会把错误谱面当内置谱面用，
 * 应当输出 FAIL 且总结论为 FAIL。证据见 evidence/M3/selftest_badchart.txt。
 */
#ifndef SELFTEST_INJECT_BAD_CHART
#define SELFTEST_INJECT_BAD_CHART   0
#endif

/* 自检用错误谱面：第 2 行轨号 9 越界，chart_parse 应返回 -2 */
static const char s_bad_chart[] =
    "# 自检用：故意错误的谱面（第 2 行轨号 9 越界）\n"
    "1500 9\n";

/* 第 1~2 节的失败数，供第 3 节打印总结论 */
static int s_early_fail;

static void print_latency_hist(void)
{
    uint32_t hist[JUDGE_LAT_BUCKETS];
    uint32_t n = judge_latency_count();
    uint32_t p95;

    judge_latency_hist(hist, JUDGE_LAT_BUCKETS);

    rt_kprintf("  判定延迟直方图（触摸中断到判定执行）: %u 个样本\n", (unsigned)n);

    if (n == 0u)
    {
        rt_kprintf("    启动阶段还没有真实触摸，属正常情况；进结算时会再打印一次实测分布\n");
        return;
    }

    rt_kprintf("    0-10ms=%u  11-20ms=%u  21-33ms=%u  34-50ms=%u  51-100ms=%u  over100ms=%u\n",
               (unsigned)hist[0], (unsigned)hist[1], (unsigned)hist[2],
               (unsigned)hist[3], (unsigned)hist[4], (unsigned)hist[5]);

    p95 = judge_latency_p95_ms();
    if (p95 > 100u)
        rt_kprintf("    P95 超过 100ms（判据 50ms）: FAIL\n");
    else if (p95 <= 50u)
        rt_kprintf("    P95 = %ums（判据 50ms）: PASS\n", (unsigned)p95);
    else
        rt_kprintf("    P95 = %ums（判据 50ms）: FAIL\n", (unsigned)p95);
}

/* 第 2 节：判定引擎（REQ-003） */
static int section_judge(void)
{
    int fail = 0;
    int f;

    rt_kprintf("\n--- [SELFTEST] 第 2 节：判定引擎（REQ-003）---\n");

    f = judge_selftest();
    rt_kprintf("  判定边界用例（窗口/一次一符/Combo/Acc/分轨）: %s\n",
               f ? "FAIL" : "PASS");
    fail += f;

    f = judge_bulk_test(50u);
    rt_kprintf("  批量判定（50 次已知时刻扫频）: %s\n", f ? "FAIL" : "PASS");
    fail += f;

    print_latency_hist();

    return fail;
}

/* 第 1 节：谱面解析（REQ-006） */
static int section_chart(void)
{
    int fail = 0;
    int bad, good;

    rt_kprintf("\n--- [SELFTEST] 第 1 节：谱面解析（REQ-006）---\n");

    /* 错误谱面先测：chart_parse 失败会把内部状态置为无效，
       若放在加载好谱面之后，会把好谱面冲掉。 */
    bad = chart_parse(s_bad_chart, -1);
    if (bad == -2)
        rt_kprintf("  错误谱面（第 2 行轨号 9 越界）: PASS（返回 -2，正确定位到第 2 行）\n");
    else
    {
        rt_kprintf("  错误谱面（第 2 行轨号 9 越界）: FAIL（期望 -2，实际 %d）\n", bad);
        fail++;
    }

#if SELFTEST_INJECT_BAD_CHART
    rt_kprintf("  [注入模式 SELFTEST_INJECT_BAD_CHART=1] 下面这项应当 FAIL\n");
    good = chart_parse(s_bad_chart, -1);
#else
    good = chart_load_builtin();
#endif

    if (good > 0)
    {
        const struct chart_info *info = chart_summary();

        rt_kprintf("  内置谱面: PASS（%d 音符，首 %ums，末 %ums，跨度 %ums，轨道 %u/%u/%u/%u）\n",
                   good,
                   (unsigned)info->first_ms, (unsigned)info->last_ms,
                   (unsigned)(info->last_ms - info->first_ms),
                   (unsigned)info->per_lane[0], (unsigned)info->per_lane[1],
                   (unsigned)info->per_lane[2], (unsigned)info->per_lane[3]);

        /* 摘要与内置谱面内容一致性：58 个音符、末音符 29500ms 是已知值 */
        if (info->notes != 58u || info->last_ms != 29500u)
        {
            rt_kprintf("  内置谱面摘要一致性（期望 58 音符 / 末 29500ms）: FAIL\n");
            fail++;
        }
        else
            rt_kprintf("  内置谱面摘要一致性（58 音符 / 末 29500ms）: PASS\n");
    }
    else
    {
        rt_kprintf("  内置谱面: FAIL（返回 %d）\n", good);
        fail++;
    }

    return fail;
}

int selftest_startup(void)
{
    int fail = 0;

    rt_kprintf("\n");
    rt_kprintf("==================== [SELFTEST] REQ-007 板上自检 ====================\n");

    /* 先判定、后谱面，理由见文件头注释 */
    fail += section_judge();
    fail += section_chart();

    s_early_fail = fail;

    rt_kprintf("\n  第 1~2 节完成，失败 %d 项；第 3 节（帧率）将在 %u 帧（约 %us）后输出\n",
               fail, (unsigned)SELFTEST_FRAME_WINDOW,
               (unsigned)(SELFTEST_FRAME_WINDOW * 33u / 1000u));

    return fail;
}

void selftest_frame_stats(void)
{
    struct loop_stat st;
    int fail = 0;

    rt_kprintf("\n--- [SELFTEST] 第 3 节：帧率统计（REQ-101）---\n");

    loop_stats(&st);
    rt_kprintf("  帧数 %u，目标 %ums/帧；平均 %ums，P95 %ums，最长 %ums\n",
               (unsigned)st.frames, (unsigned)st.target_ms,
               (unsigned)st.avg_ms, (unsigned)st.p95_ms, (unsigned)st.max_ms);
    rt_kprintf("  其中绘制 %ums、提交等待 %ums；超时帧 %u\n",
               (unsigned)st.avg_draw_ms, (unsigned)st.avg_flush_ms,
               (unsigned)st.miss);

    if (st.p95_ms == 0u)
    {
        rt_kprintf("  帧率 P95: 无样本（帧数不足），本次不判定\n");
    }
    else if (st.p95_ms <= SELFTEST_P95_LIMIT_MS)
    {
        rt_kprintf("  帧率 P95（判据不超过 %ums）: PASS\n",
                   (unsigned)SELFTEST_P95_LIMIT_MS);
    }
    else
    {
        rt_kprintf("  帧率 P95（判据不超过 %ums）: FAIL（实测 %ums）\n",
                   (unsigned)SELFTEST_P95_LIMIT_MS, (unsigned)st.p95_ms);
        fail++;
    }

    rt_kprintf("\n==================== [SELFTEST] 总结论: %s ====================\n",
               (s_early_fail + fail) ? "FAIL" : "PASS");
    rt_kprintf("  第 1~2 节失败 %d 项，第 3 节失败 %d 项\n",
               s_early_fail, fail);
    rt_kprintf("  注：帧率判据已按 CR-04 由 33ms 放宽到 34ms，打点分辨率 1ms，见 docs/06 的 R-10\n\n");
}

void selftest_latency_report(void)
{
    if (judge_latency_count() == 0u)
        return;     /* 这一局没产生样本，不打印，避免刷屏 */

    rt_kprintf("[SELFTEST] 判定延迟实测（本局结束）:\n");
    print_latency_hist();
}
