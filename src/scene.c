/*
 * scene.c —— 场景状态机与三页渲染（REQ-001 / REQ-003 / REQ-004 / REQ-005）
 *
 * 对应 docs/02 §3 的 scene 模块、§4.1 的 scene_* 接口与 §4.2 的状态机表。
 *
 * 状态流转（docs/02 §4.2）：
 *   TITLE ──触屏──> PLAYING ──最后一个音符判完──> RESULT ──触屏──> PLAYING
 *                                                 └─超时 30s──> TITLE
 *
 * 线程模型：触摸事件只由读点线程写环形队列，**只有帧循环线程 pop**
 * （单生产者单消费者，无锁）。本文件因此不含任何互斥量——所有状态都由
 * 帧循环线程读写，不存在竞争。
 *
 * 时间基准：P0 无声，用系统毫秒。进入 PLAYING 时记 play_start_ms，
 * 谱面时间 chart_ms = now_ms - play_start_ms，与 note.hit_ms 同一坐标系。
 * P1（REQ-008）改成以音频播放进度为基准时，只需替换 s_chart_ms 的来源。
 */
#include "app.h"
#include <string.h>

#define LANE_COUNT          4
#define NOTE_H              12          /* 音符高度（px） */
#define NOTE_MARGIN         4           /* 音符左右留白（px） */
#define JUDGE_LINE_OFFSET   70          /* 判定线距屏幕底部的高度 */
#define NOTE_SPEED_PX_S     300         /* 落速：px/秒，见 docs/02 §4.2 */
#define NOTE_FADE_MS        200         /* 过判定线后继续画多久（视觉余韵） */
#define RESULT_TIMEOUT_MS   30000u      /* 结算页 30s 无操作回主界面 */

/*
 * HUD 底板（docs/02 §7 决策，2026-10-17 上板后由学生选定方案 A）：
 * 390px 宽被 4 轨铺满，没侧边可放 HUD；判定线下方只剩 69px 且是手指点击区。
 * 所以 HUD 留在左上角，但加一块比背景亮一档的底板，音符从板下穿过，
 * 这样文字永远可读，不会像上板时那样被绿色音符盖住。
 */
#define HUD_PAD             4
#define HUD_TEXT_X          8
#define HUD_TEXT_Y          8
#define HUD_LINE_H          22          /* 行距：字高 7*2=14 + 间隙 8 */
#define HUD_W               132         /* 底板宽：能容纳 "SCORE 5800" / "ACC 100.00" */
#define HUD_H               68          /* 底板高：3 行文字（8..66） + 上下留白 */

static enum scene_id s_scene = SCENE_TITLE;
static uint32_t s_play_start_ms;
static uint32_t s_result_ms;
static uint32_t s_chart_ms;             /* 当前谱面时间（只在 PLAYING 有意义） */

/* ---------------- 状态切换 ---------------- */

static void enter_title(void)
{
    s_scene = SCENE_TITLE;
    rt_kprintf("[SCENE] -> TITLE at %ums\n",
               (unsigned)rt_tick_get_millisecond());
}

/*
 * tap_ms：触发本次切换的触摸中断时刻（无触摸触发时传 0）。
 * 必须记录它，否则 TC-001 的「点击后 1s 内进入演奏」只靠切换时刻是无法验证的
 * （只能看到"什么时候切了"，不知道"什么时候点的"）。
 */
static void enter_playing(uint32_t now_ms, uint32_t tap_ms)
{
    int32_t latency = (int32_t)(now_ms - tap_ms);

    if (latency < 0)
        latency = 0;    /* 事件入队可能在帧开始之后，防下溢 */

    /*
     * judge_reset() 只把音符 state 与统计清零，不动音符本身，
     * 所以重开一局不需要重新解析谱面（谱面解析只在启动时做一次）。
     */
    judge_reset();
    s_play_start_ms = now_ms;
    s_chart_ms = 0;
    s_scene = SCENE_PLAYING;
    rt_kprintf("[SCENE] -> PLAYING at %ums (notes=%u, tap=%ums, latency=%dms)\n",
               (unsigned)now_ms, (unsigned)chart_note_count(),
               (unsigned)tap_ms, (int)latency);
}

static void enter_result(uint32_t now_ms)
{
    struct judge_stat st;

    judge_stats(&st);
    s_result_ms = now_ms;
    s_scene = SCENE_RESULT;

    /* TC-005 要求「结算四项数据与过程日志一致」，这里把四项全部打出来 */
    rt_kprintf("[SCENE] -> RESULT at %ums score=%u maxcombo=%u acc=%u.%02u%% "
               "P=%u G=%u M=%u\n",
               (unsigned)now_ms,
               (unsigned)st.score, (unsigned)st.max_combo,
               (unsigned)(st.acc_x100 / 100u), (unsigned)(st.acc_x100 % 100u),
               (unsigned)st.perfect, (unsigned)st.good, (unsigned)st.miss);
}

/* ---------------- 更新（逻辑） ---------------- */

void scene_update(uint32_t now_ms)
{
    struct touch_ev ev;
    int w = fb_width();
    int lane_w = (w > 0) ? (w / LANE_COUNT) : 1;

    if (lane_w < 1)
        lane_w = 1;

    /* 先把谱面时间推进到本帧，事件分派时用的才是当前时刻 */
    if (s_scene == SCENE_PLAYING)
        s_chart_ms = now_ms - s_play_start_ms;

    /* 出队所有积压事件（每帧清空队列，避免延迟累积） */
    while (touch_pop_event(&ev) == 0)
    {
        switch (s_scene)
        {
        case SCENE_TITLE:
            /* P0 主界面只有「开始」一个可点元素，整屏任意位置都算 */
            enter_playing(now_ms, ev.t_ms);
            break;

        case SCENE_PLAYING:
        {
            int lane = (int)ev.x / lane_w;
            enum judge_result r;

            if (lane >= LANE_COUNT)
                lane = LANE_COUNT - 1;
            if (lane < 0)
                lane = 0;

            /* 用谱面时间判定：note.hit_ms 与 s_chart_ms 同一坐标系 */
            r = judge_touch((uint8_t)lane, s_chart_ms);

            /*
             * 只打印真正消耗了音符的点击。JUDGE_NONE 是「附近没有可判音符」，
             * 属于正常情况（例如音符还没落下来时就点），打印会刷屏。
             */
            if (r != JUDGE_NONE)
            {
                rt_kprintf("[HIT] lane=%d tap=%ums chart=%ums -> %s\n",
                           lane, (unsigned)ev.t_ms, (unsigned)s_chart_ms,
                           judge_result_name(r));
            }
            break;
        }

        case SCENE_RESULT:
            /* P0 结算页只有「重开」一个可点元素 */
            enter_playing(now_ms, ev.t_ms);
            break;

        default:
            break;
        }
    }

    if (s_scene == SCENE_PLAYING)
    {
        struct judge_stat st;

        /* 超时未判的音符置为 MISS、Combo 归零 */
        judge_tick(s_chart_ms);
        judge_stats(&st);

        /* 最后一个音符判完即进结算（REQ-005） */
        if (chart_note_count() > 0u && st.judged >= chart_note_count())
            enter_result(now_ms);
    }
    else if (s_scene == SCENE_RESULT)
    {
        if (now_ms - s_result_ms >= RESULT_TIMEOUT_MS)
            enter_title();
    }
}

/* ---------------- 渲染（只写背缓冲） ---------------- */

static void render_title(uint32_t now_ms)
{
    int w = fb_width();
    int h = fb_height();
    static const char *title = "HSP RHYTHM";
    static const char *hint = "TAP TO START";
    int tw = gfx_text_width(title, 3);
    int hw = gfx_text_width(hint, 2);
    int ty = h / 3;

    fb_clear(COL_BG);

    gfx_text((w - tw) / 2, ty, title, COL_TEXT, 3);

    /* 提示文字 1Hz 闪烁，避免像静止画面 */
    if (((now_ms / 500u) & 1u) == 0u)
        gfx_text((w - hw) / 2, ty + 7 * 3 + 30, hint, COL_NOTE, 2);

    gfx_text((w - gfx_text_width("SF32LB52 390X450", 1)) / 2, h - 24,
             "SF32LB52 390X450", COL_DIM, 1);
}

static void render_hud(void)
{
    struct judge_stat st;
    char buf[24];

    judge_stats(&st);

    /* 先铺底板，再写文字：音符会从板下穿过，文字不会被盖住 */
    gfx_fill_rect(HUD_PAD, HUD_PAD, HUD_PAD + HUD_W - 1, HUD_PAD + HUD_H - 1,
                  COL_HUD_BG);

    rt_snprintf(buf, sizeof(buf), "SCORE %u", (unsigned)st.score);
    gfx_text(HUD_TEXT_X, HUD_TEXT_Y, buf, COL_TEXT, 2);

    rt_snprintf(buf, sizeof(buf), "COMBO %u", (unsigned)st.combo);
    gfx_text(HUD_TEXT_X, HUD_TEXT_Y + HUD_LINE_H, buf, COL_TEXT, 2);

    rt_snprintf(buf, sizeof(buf), "ACC %u.%02u",
                (unsigned)(st.acc_x100 / 100u),
                (unsigned)(st.acc_x100 % 100u));
    gfx_text(HUD_TEXT_X, HUD_TEXT_Y + HUD_LINE_H * 2, buf, COL_TEXT, 2);
}

static void render_playing(void)
{
    int w = fb_width();
    int h = fb_height();
    int lane_w = (w > 0) ? (w / LANE_COUNT) : 1;
    int judge_y = h - JUDGE_LINE_OFFSET;
    int note_w = lane_w - NOTE_MARGIN * 2;
    const struct note *notes = chart_notes();
    uint32_t n = chart_note_count();

    if (lane_w < 1)
        lane_w = 1;
    if (note_w < 2)
        note_w = 2;

    fb_clear(COL_BG);

    /* 轨道分隔线 */
    for (int i = 1; i < LANE_COUNT; i++)
        gfx_vline(i * lane_w, 0, h - 1, COL_LANE);

    /* 判定线（两像素高，便于肉眼定位） */
    gfx_hline(0, w - 1, judge_y, COL_JUDGE);
    gfx_hline(0, w - 1, judge_y + 1, COL_JUDGE);

    /*
     * 音符位置只由「谱面时间差」决定，不查 state：
     *   y = judge_y - (hit_ms - chart_ms) * 落速
     * 这样音符在 hit_ms 那一刻正好压在判定线上。
     * 过线 NOTE_FADE_MS 之后不再画（相当于音符已消失）。
     */
    for (uint32_t i = 0; i < n; i++)
    {
        int32_t dt = (int32_t)notes[i].hit_ms - (int32_t)s_chart_ms;
        int y, x0, x1;

        if (dt < -(int32_t)NOTE_FADE_MS)
            continue;

        /* 还在屏幕上方看不见的位置，跳过 */
        if (dt > (int32_t)(((int32_t)h + NOTE_H) * 1000 / NOTE_SPEED_PX_S))
            continue;

        y = judge_y - (int)(dt * NOTE_SPEED_PX_S / 1000);

        if (y + NOTE_H <= 0 || y >= h)
            continue;

        x0 = (int)notes[i].lane * lane_w + NOTE_MARGIN;
        x1 = x0 + note_w - 1;
        gfx_fill_rect(x0, y, x1, y + NOTE_H - 1, COL_NOTE);
    }

    render_hud();
}

static void render_result(void)
{
    int w = fb_width();
    struct judge_stat st;
    char buf[32];
    int y;
    static const char *title = "RESULT";
    static const char *hint = "TAP TO RETRY";

    judge_stats(&st);

    fb_clear(COL_BG);

    gfx_text((w - gfx_text_width(title, 3)) / 2, 36, title, COL_TEXT, 3);
    y = 36 + 7 * 3 + 34;

    rt_snprintf(buf, sizeof(buf), "SCORE %u", (unsigned)st.score);
    gfx_text((w - gfx_text_width(buf, 2)) / 2, y, buf, COL_NOTE, 2);
    y += 26;

    rt_snprintf(buf, sizeof(buf), "MAX COMBO %u", (unsigned)st.max_combo);
    gfx_text((w - gfx_text_width(buf, 2)) / 2, y, buf, COL_TEXT, 2);
    y += 26;

    rt_snprintf(buf, sizeof(buf), "ACC %u.%02u%%",
                (unsigned)(st.acc_x100 / 100u),
                (unsigned)(st.acc_x100 % 100u));
    gfx_text((w - gfx_text_width(buf, 2)) / 2, y, buf, COL_TEXT, 2);
    y += 26;

    /* 档位计数拆成两行，390px 宽下 scale=2 一行放不下 P/G/M 三个数 */
    rt_snprintf(buf, sizeof(buf), "PERFECT %u  GOOD %u",
                (unsigned)st.perfect, (unsigned)st.good);
    gfx_text((w - gfx_text_width(buf, 2)) / 2, y, buf, COL_TEXT, 2);
    y += 26;

    rt_snprintf(buf, sizeof(buf), "MISS %u", (unsigned)st.miss);
    gfx_text((w - gfx_text_width(buf, 2)) / 2, y, buf, COL_DIM, 2);

    gfx_text((w - gfx_text_width(hint, 2)) / 2, fb_height() - 40, hint,
             COL_NOTE, 2);
}

void scene_render(void)
{
    switch (s_scene)
    {
    case SCENE_TITLE:
        render_title(rt_tick_get_millisecond());
        break;

    case SCENE_PLAYING:
        render_playing();
        break;

    case SCENE_RESULT:
        render_result();
        break;

    default:
        fb_clear(COL_BG);
        break;
    }
}

void scene_init(void)
{
    s_scene = SCENE_TITLE;
    s_play_start_ms = 0;
    s_result_ms = 0;
    s_chart_ms = 0;

    /* TC-001 要求「串口打印状态切换时间戳」，上电进入主界面也算一次切换 */
    rt_kprintf("[SCENE] -> TITLE at %ums (boot)\n",
               (unsigned)rt_tick_get_millisecond());
}

enum scene_id scene_current(void)
{
    return s_scene;
}
