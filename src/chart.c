/*
 * chart.c —— 文本谱面解析（REQ-006）
 *
 * 对应 docs/02 §3 的 chart 模块与 §4.1/§4.2 的接口与格式细则。
 *
 * 为什么谱面是文本而不是数组字面量：
 *   REQ-006 明确要求「谱面以文本表示」，TC-006 要求「解析失败有明确报错 + 打印解析摘要」。
 *   用文本才能在后续 P1（REQ-008）直接换成从 TF 卡读文件，解析这段代码不用改。
 *
 * 格式（docs/02 §4.2）：
 *   每行： <时间戳ms> <轨号 0-3>
 *   - 两者之间用空白（空格/制表符），前后允许多余空白
 *   - '#' 起到行尾为注释；空行或纯注释行跳过
 *   - 时间戳 0..3600000；轨号只能 0..3
 *   - **时间戳必须单调不减**：乱序会让 judge_touch() 的「最近待判音符」选错对象，
 *     这种错误必须在加载时暴露，不能留到演奏中
 *   - 出错返回 -(行号)，行号从 1 起算（与 TC-006 的判据对齐）
 *
 * 解析失败时不会破坏调用方已加载的判定数据（judge 内部有自己的副本），
 * 但会把 chart_summary() 置为 0 音符状态，表示「当前谱面无效」。
 */
#include "app.h"
#include <string.h>

#define CHART_MAX_MS    3600000u    /* 1 小时，见 docs/02 §4.2 */

/* 解析结果写在这里（judge 内部另有独立副本，两者互不影响） */
static struct note s_notes[JUDGE_MAX_NOTES];
static struct chart_info s_info;

/*
 * 内置谱面：120 BPM（每音符间隔 500ms），4 轨轮转。
 * 58 个音符，首音符 1000ms、末音符 29500ms，对应 TC-101 的「连续演奏 30s」。
 * 轨号序列 0 2 1 3 / 1 3 0 2 / 2 0 3 1 / 3 1 2 0 循环，避免同轨连击过于机械，
 * 同时保证同一轨相邻音符间隔 >= 2000ms，不会踩到 ±120ms 的判定窗口。
 */
static const char s_builtin_chart[] =
    "# HSP-01 内置谱面（P0 单曲，REQ-006）\n"
    "# 每行： <时间戳ms> <轨号 0-3>\n"
    "# 节奏：120 BPM，每音符间隔 500ms；共 58 个音符，末音符 29500ms\n"
    "1000 0\n"
    "1500 2\n"
    "2000 1\n"
    "2500 3\n"
    "3000 1\n"
    "3500 3\n"
    "4000 0\n"
    "4500 2\n"
    "5000 2\n"
    "5500 0\n"
    "6000 3\n"
    "6500 1\n"
    "7000 3\n"
    "7500 1\n"
    "8000 2\n"
    "8500 0\n"
    "9000 0\n"
    "9500 2\n"
    "10000 1\n"
    "10500 3\n"
    "11000 1\n"
    "11500 3\n"
    "12000 0\n"
    "12500 2\n"
    "13000 2\n"
    "13500 0\n"
    "14000 3\n"
    "14500 1\n"
    "15000 3\n"
    "15500 1\n"
    "16000 2\n"
    "16500 0\n"
    "17000 0\n"
    "17500 2\n"
    "18000 1\n"
    "18500 3\n"
    "19000 1\n"
    "19500 3\n"
    "20000 0\n"
    "20500 2\n"
    "21000 2\n"
    "21500 0\n"
    "22000 3\n"
    "22500 1\n"
    "23000 3\n"
    "23500 1\n"
    "24000 2\n"
    "24500 0\n"
    "25000 0\n"
    "25500 2\n"
    "26000 1\n"
    "26500 3\n"
    "27000 1\n"
    "27500 3\n"
    "28000 0\n"
    "28500 2\n"
    "29000 2\n"
    "29500 0\n";

/* 解析一个十进制无符号整数，成功后 pp 指向数字之后。失败返回 -1。 */
static int parse_uint(const char **pp, const char *end, uint32_t *out)
{
    const char *p = *pp;
    uint32_t v = 0;
    int got = 0;

    while (p < end && *p >= '0' && *p <= '9')
    {
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 100000000u)     /* 明显溢出，直接判错，避免绕回成小数字 */
            return -1;
        p++;
        got = 1;
    }

    if (!got)
        return -1;

    *pp = p;
    *out = v;
    return 0;
}

static int is_blank(char c)
{
    return (c == ' ' || c == '\t') ? 1 : 0;
}

int chart_parse(const char *text, int len)
{
    const char *p, *end;
    int line = 0;
    uint32_t n = 0;
    uint32_t prev_ms = 0;
    int have_prev = 0;
    int rc = 0;

    memset(&s_info, 0, sizeof(s_info));

    if (!text)
        return -1;

    p = text;
    end = (len < 0) ? (text + strlen(text)) : (text + len);

    while (p < end)
    {
        const char *eol = p;
        const char *q;

        while (eol < end && *eol != '\n' && *eol != '\r')
            eol++;

        line++;

        /* 跳过行首空白 */
        q = p;
        while (q < eol && is_blank(*q))
            q++;

        /* 空行或纯注释行：跳过 */
        if (q < eol && *q != '#')
        {
            uint32_t t_ms = 0;
            uint32_t lane = 0;

            if (parse_uint(&q, eol, &t_ms) != 0)
            {
                rc = -line;
                goto out;
            }

            /* 时间戳与轨号之间必须有空白 */
            if (q >= eol || !is_blank(*q))
            {
                rc = -line;
                goto out;
            }

            while (q < eol && is_blank(*q))
                q++;

            if (parse_uint(&q, eol, &lane) != 0)
            {
                rc = -line;
                goto out;
            }

            /* 轨号之后只能是空白或注释 */
            while (q < eol && is_blank(*q))
                q++;

            if (q < eol && *q != '#')
            {
                rc = -line;
                goto out;
            }

            if (t_ms > CHART_MAX_MS || lane > 3u)
            {
                rc = -line;
                goto out;
            }

            /* 单调不减：乱序会让判定选错音符 */
            if (have_prev && t_ms < prev_ms)
            {
                rc = -line;
                goto out;
            }

            if (n >= JUDGE_MAX_NOTES)
            {
                rc = -line;
                goto out;
            }

            s_notes[n].hit_ms = t_ms;
            s_notes[n].lane = (uint8_t)lane;
            s_notes[n].state = 0;
            n++;
            prev_ms = t_ms;
            have_prev = 1;
        }

        /* 前进到下一行（容忍 \r\n 与 \n） */
        p = eol;
        if (p < end && *p == '\r')
            p++;
        if (p < end && *p == '\n')
            p++;
    }

    rc = (int)n;

out:
    if (rc >= 0)
    {
        s_info.notes = n;
        if (n > 0)
        {
            s_info.first_ms = s_notes[0].hit_ms;
            s_info.last_ms = s_notes[n - 1].hit_ms;
            for (uint32_t i = 0; i < n; i++)
                s_info.per_lane[s_notes[i].lane]++;
        }
    }
    else
    {
        memset(&s_info, 0, sizeof(s_info));
    }

    return rc;
}

int chart_load_builtin(void)
{
    int n = chart_parse(s_builtin_chart, -1);
    const struct chart_info *info = &s_info;

    if (n < 0)
    {
        rt_kprintf("[CHART] ERROR: builtin chart syntax error at line %d\n", -n);
        return n;
    }

    if (n == 0)
    {
        rt_kprintf("[CHART] ERROR: builtin chart has no notes\n");
        return -1;
    }

    if (judge_load(s_notes, (uint32_t)n) != 0)
        return -1;

    rt_kprintf("[CHART] builtin loaded: %d notes, first=%ums last=%ums "
               "span=%ums lanes=%u/%u/%u/%u\n",
               n,
               (unsigned)info->first_ms, (unsigned)info->last_ms,
               (unsigned)(info->last_ms - info->first_ms),
               (unsigned)info->per_lane[0], (unsigned)info->per_lane[1],
               (unsigned)info->per_lane[2], (unsigned)info->per_lane[3]);
    return n;
}

const struct note *chart_notes(void)
{
    return s_notes;
}

uint32_t chart_note_count(void)
{
    return s_info.notes;
}

const struct chart_info *chart_summary(void)
{
    return &s_info;
}
