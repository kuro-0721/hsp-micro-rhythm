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
 *
 * 【硬性约束】本文件所有 rt_kprintf 的格式串**必须只用 ASCII 英文**。
 * 2026-10-17 上板实测：往 rt_kprintf 里写中文会在串口输出中全部变成 '?'，
 * 因为 GCC 对源文件的字符集转换会把非 ASCII 字符字面量替掉。
 * 注释里写中文没问题（不参与编译），但**打进二进制的字符串不行**。
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

    rt_kprintf("  judge latency histogram (touch IRQ to judge): %u samples\n", (unsigned)n);

    if (n == 0u)
    {
        rt_kprintf("    no real touch at boot - normal; printed again at RESULT\n");
        return;
    }

    rt_kprintf("    0-10=%u  11-20=%u  21-33=%u  34-50=%u  51-100=%u  over100=%u (ms)\n",
               (unsigned)hist[0], (unsigned)hist[1], (unsigned)hist[2],
               (unsigned)hist[3], (unsigned)hist[4], (unsigned)hist[5]);

    p95 = judge_latency_p95_ms();
    if (p95 > 100u)
        rt_kprintf("    p95 above 100ms (limit 50ms): FAIL\n");
    else if (p95 <= 50u)
        rt_kprintf("    p95 = %ums (limit 50ms): PASS\n", (unsigned)p95);
    else
        rt_kprintf("    p95 = %ums (limit 50ms): FAIL\n", (unsigned)p95);
}

/* 第 2 节：判定引擎（REQ-003） */
static int section_judge(void)
{
    int fail = 0;
    int f;

    rt_kprintf("\n--- [SELFTEST] section 2/3: judge engine (REQ-003) ---\n");

    f = judge_selftest();
    rt_kprintf("  judge edge cases (window/one-per-tap/combo/acc/lane): %s\n",
               f ? "FAIL" : "PASS");
    fail += f;

    f = judge_bulk_test(50u);
    rt_kprintf("  bulk judge (50 known click offsets): %s\n", f ? "FAIL" : "PASS");
    fail += f;

    print_latency_hist();

    return fail;
}

/* 第 1 节：谱面解析（REQ-006） */
static int section_chart(void)
{
    int fail = 0;
    int bad, good;

    rt_kprintf("\n--- [SELFTEST] section 1/3: chart parsing (REQ-006) ---\n");

    /* 错误谱面先测：chart_parse 失败会把内部状态置为无效，
       若放在加载好谱面之后，会把好谱面冲掉。 */
    bad = chart_parse(s_bad_chart, -1);
    if (bad == -2)
        rt_kprintf("  bad chart (lane 9 on line 2): PASS (returned -2, reported line 2)\n");
    else
    {
        rt_kprintf("  bad chart (lane 9 on line 2): FAIL (expect -2, got %d)\n", bad);
        fail++;
    }

#if SELFTEST_INJECT_BAD_CHART
    rt_kprintf("  [INJECT MODE SELFTEST_INJECT_BAD_CHART=1] next item should FAIL\n");
    good = chart_parse(s_bad_chart, -1);
#else
    good = chart_load_builtin();
#endif

    if (good > 0)
    {
        const struct chart_info *info = chart_summary();

        rt_kprintf("  builtin chart: PASS (%d notes, first=%ums last=%ums span=%ums lanes=%u/%u/%u/%u)\n",
                   good,
                   (unsigned)info->first_ms, (unsigned)info->last_ms,
                   (unsigned)(info->last_ms - info->first_ms),
                   (unsigned)info->per_lane[0], (unsigned)info->per_lane[1],
                   (unsigned)info->per_lane[2], (unsigned)info->per_lane[3]);

        /* 摘要与内置谱面内容一致性：58 个音符、末音符 29500ms 是已知值 */
        if (info->notes != 58u || info->last_ms != 29500u)
        {
            rt_kprintf("  builtin chart summary (expect 58 notes / last 29500ms): FAIL\n");
            fail++;
        }
        else
            rt_kprintf("  builtin chart summary (58 notes / last 29500ms): PASS\n");
    }
    else
    {
        rt_kprintf("  builtin chart: FAIL (returned %d)\n", good);
        fail++;
    }

    return fail;
}

int selftest_startup(void)
{
    int fail = 0;

    rt_kprintf("\n");
    rt_kprintf("==================== [SELFTEST] REQ-007 boot self-test ====================\n");

    /* 先判定、后谱面，理由见文件头注释 */
    fail += section_judge();
    fail += section_chart();

    s_early_fail = fail;

    rt_kprintf("\n  sections 1-2 done, %d failure(s); "
               "section 3 (frame rate) in %u frames (~%us)\n",
               fail, (unsigned)SELFTEST_FRAME_WINDOW,
               (unsigned)(SELFTEST_FRAME_WINDOW * 33u / 1000u));

    return fail;
}

void selftest_frame_stats(void)
{
    struct loop_stat st;
    int fail = 0;

    rt_kprintf("\n--- [SELFTEST] section 3/3: frame rate (REQ-101) ---\n");

    loop_stats(&st);
    rt_kprintf("  frames=%u target=%ums avg=%ums p95=%ums max=%ums\n",
               (unsigned)st.frames, (unsigned)st.target_ms,
               (unsigned)st.avg_ms, (unsigned)st.p95_ms, (unsigned)st.max_ms);
    rt_kprintf("  draw=%ums flush=%ums overrun_frames=%u\n",
               (unsigned)st.avg_draw_ms, (unsigned)st.avg_flush_ms,
               (unsigned)st.miss);

    if (st.p95_ms == 0u)
    {
        rt_kprintf("  p95: no samples (frame count too low), skipped\n");
    }
    else if (st.p95_ms <= SELFTEST_P95_LIMIT_MS)
    {
        rt_kprintf("  p95 (limit %ums): PASS\n",
                   (unsigned)SELFTEST_P95_LIMIT_MS);
    }
    else
    {
        rt_kprintf("  p95 (limit %ums): FAIL (measured %ums)\n",
                   (unsigned)SELFTEST_P95_LIMIT_MS, (unsigned)st.p95_ms);
        fail++;
    }

    rt_kprintf("\n==================== [SELFTEST] RESULT: %s ====================\n",
               (s_early_fail + fail) ? "FAIL" : "PASS");
    rt_kprintf("  section 1-2 failures: %d, section 3 failures: %d\n",
               s_early_fail, fail);
    rt_kprintf("  note: p95 limit widened 33ms to 34ms by CR-04; "
               "tick resolution 1ms, see docs/06 R-10\n\n");
}

void selftest_latency_report(void)
{
    if (judge_latency_count() == 0u)
        return;     /* 这一局没产生样本，不打印，避免刷屏 */

    rt_kprintf("[SELFTEST] measured judge latency (this run ended):\n");
    print_latency_hist();
}
