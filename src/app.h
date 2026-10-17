/*
 * app.h —— M3 各模块之间的公共接口
 *
 * 对应 docs/02-方案设计说明.md §3（模块划分）与 §4（接口设计）。
 * 只有 main.c 定义 main()；其它模块只提供 app_start() 与各自接口。
 *
 * 命名约定：
 *   fb_*    渲染缓冲与提交（src/fb.c）
 *   gfx_*   绘制原语，只写背缓冲、不提交（src/gfx.c）
 *   game_*  固定帧循环与帧统计（src/game_loop.c）
 */
#ifndef APP_H
#define APP_H

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <stdint.h>

/* ---------------- 颜色（RGB565） ---------------- */

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BG      RGB565(16, 16, 16)      /* 背景：深灰 */
#define COL_LANE    RGB565(48, 48, 48)      /* 轨道分隔线 */
#define COL_JUDGE   RGB565(255, 255, 255)   /* 判定线 */
#define COL_NOTE    RGB565(0, 255, 128)     /* 音符 */
#define COL_TEXT    RGB565(255, 255, 255)   /* 文本 */
#define COL_DIM     RGB565(120, 120, 120)

/* ---------------- fb：整屏帧缓冲（ADR-0003 单缓冲） ---------------- */

/* 打开 lcd、读取信息、设 RGB565、开背光、在 PSRAM 分配整屏缓冲。成功返回 0。 */
int       fb_init(void);

/* 返回可写的整屏缓冲（RGB565，宽×高）。未初始化时返回 RT_NULL。 */
uint16_t *fb_backbuffer(void);

/* 提交整屏并等待刷屏完成（dcache clean + set_window + draw_rect_async + 等回调）。 */
int       fb_present(void);

/* 脏区提交（预留接口）：只提交 [x0,x1]×[y0,y1] 区域。坐标会被裁剪到屏幕内。 */
int       fb_flush_rect(int x0, int y0, int x1, int y1);

void      fb_clear(uint16_t color);
int       fb_width(void);
int       fb_height(void);

/* ---------------- gfx：绘制原语（只写缓冲，不提交） ---------------- */

void gfx_fill_rect(int x0, int y0, int x1, int y1, uint16_t color);
void gfx_hline(int x0, int x1, int y, uint16_t color);
void gfx_vline(int x, int y0, int y1, uint16_t color);

/* ---------------- 触摸输入（src/input_touch.c） ---------------- */

#define TP_QUEUE_LEN    32

struct touch_ev
{
    uint32_t t_ms;      /* 中断时刻（rt_tick_get_millisecond，分辨率 1ms） */
    uint16_t x, y;
};

/* 打开 "touch" 设备、注册 rx_indicate、启动读点线程。成功返回 0。 */
int touch_init(void);

/* 取一个已完成读点的事件。0 = 取到，-1 = 队列空。 */
int touch_pop_event(struct touch_ev *out);

/* 【临时】触摸→判定的桥接线程（src/input_bridge.c，第 3 步删除） */
int input_bridge_start(void);

/* ---------------- 判定引擎（src/judge.c） ---------------- */

#define JUDGE_PERFECT_MS    60u
#define JUDGE_GOOD_MS       120u
#define JUDGE_MAX_NOTES     256u

enum judge_result
{
    JUDGE_NONE = 0,     /* 没有可判定的音符 */
    JUDGE_PERFECT,
    JUDGE_GOOD,
    JUDGE_MISS,
};

struct note
{
    uint32_t hit_ms;    /* 该音符应当被命中的时刻 */
    uint8_t  lane;      /* 0..3 */
    uint8_t  state;     /* 0 = 待判定，1 = 已判定 */
};

struct judge_stat
{
    uint32_t perfect, good, miss;
    uint32_t combo, max_combo;
    uint32_t score;
    uint32_t judged;    /* 已判定音符数 */
    uint32_t acc_x100;  /* Acc 百分比 ×100（整数，避免浮点） */
};

int  judge_init(void);
int  judge_load(const struct note *notes, uint32_t n);
enum judge_result judge_touch(uint8_t lane, uint32_t t_ms);
uint32_t judge_tick(uint32_t now_ms);
void judge_stats(struct judge_stat *out);
void judge_reset(void);
int  judge_selftest(void);          /* 纯逻辑自检：返回 0 = 全部 PASS */
int  judge_bulk_test(uint32_t count); /* 批量判定测试（TC-003 的 ≥50 次），返回失败数 */

const char *judge_result_name(enum judge_result r);

/* ---------------- game_loop：固定帧循环与统计 ---------------- */

struct loop_stat
{
    uint32_t frames;        /* 累计帧数 */
    uint32_t avg_ms;        /* 平均单帧耗时（draw + flush） */
    uint32_t p95_ms;        /* 95 分位单帧耗时 */
    uint32_t max_ms;        /* 最长单帧 */
    uint32_t avg_draw_ms;   /* 平均 CPU 绘制耗时 */
    uint32_t avg_flush_ms;  /* 平均提交等待耗时 */
    uint32_t miss;          /* 超时帧数（单帧 > FRAME_MS + 容差） */
    uint32_t target_ms;     /* 目标帧间隔 */
};

/* 初始化（依赖 fb_init 已成功）并启动帧循环线程。 */
int  game_loop_start(void);

/* 读取统计快照。 */
void loop_stats(struct loop_stat *out);

/* ---------------- 应用入口 ---------------- */

/* 由 main.c 调用：初始化 LCD → 启动帧循环。成功返回 0。 */
int app_start(void);

#endif /* APP_H */
