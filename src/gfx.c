/*
 * gfx.c —— 绘制原语（只写背缓冲，不提交）
 *
 * 设计约定（docs/02 §3）：
 *   - 本模块不知道「什么时候刷屏」，所有函数只改 framebuffer 内容；
 *     提交由调用方（game_loop）在帧末统一调用 fb_present() / fb_flush_rect()。
 *   - 所有坐标都会被裁剪到屏幕范围内，越界不会写到缓冲外。
 *   - 文字（gfx_text）本阶段未实现：M3 第 1 步只需要验证「音符密度下的帧率」，
 *     文字留给第 3 步（主界面/结算页）时补，避免现在引入字模表拖慢进度。
 */
#include "app.h"

/* 取缓冲与尺寸。每帧调用几百次，尺寸走静态缓存避免重复的 rt_device_control 路径。 */
static inline uint16_t *g_buf(int *w, int *h)
{
    *w = fb_width();
    *h = fb_height();
    return fb_backbuffer();
}

void gfx_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    int w, h;
    uint16_t *buf = g_buf(&w, &h);

    if (!buf)
        return;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w - 1)
        x1 = w - 1;
    if (y1 > h - 1)
        y1 = h - 1;
    if (x0 > x1 || y0 > y1)
        return;

    rt_uint32_t span = (rt_uint32_t)(x1 - x0 + 1);
    rt_uint32_t pair = ((rt_uint32_t)color << 16) | color;

    for (int y = y0; y <= y1; y++)
    {
        uint16_t *row = buf + (rt_uint32_t)y * (rt_uint32_t)w + (rt_uint32_t)x0;

        /* 起点对齐到 4 字节时按 32 位写，一次两个像素 */
        if ((x0 & 1) == 0 && span >= 2u)
        {
            rt_uint32_t *p32 = (rt_uint32_t *)row;
            rt_uint32_t n32 = span >> 1;
            for (rt_uint32_t i = 0; i < n32; i++)
                p32[i] = pair;
            if (span & 1u)
                row[span - 1] = color;
        }
        else
        {
            for (rt_uint32_t i = 0; i < span; i++)
                row[i] = color;
        }
    }
}

void gfx_hline(int x0, int x1, int y, uint16_t color)
{
    gfx_fill_rect(x0, y, x1, y, color);
}

void gfx_vline(int x, int y0, int y1, uint16_t color)
{
    gfx_fill_rect(x, y0, x, y1, color);
}
