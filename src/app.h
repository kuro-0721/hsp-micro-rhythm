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
