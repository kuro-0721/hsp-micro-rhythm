/*
 * M2 关键技术验证原型（临时文件：M3 会按 docs/02 拆到 fb.c / input_touch.c / game_loop.c）
 *
 * 目的（对应 docs/03 的用例）：
 *   1) 点亮屏幕并读回分辨率      → 验证假设 A-01、TC-002 前置
 *   2) 刷屏帧间隔统计（P95/超时）→ TC-101 / REQ-101
 *   3) 触摸事件双时间戳（中断时刻 vs 读点完成时刻）→ TC-003 / TC-104 的测量基础
 *
 * 设计取舍：
 *   - 按学生决定先走"分行刷"（band flush），不做双缓冲 framebuffer：
 *     内存占用小（一个 390×60×2B ≈ 45.7KB 的 band 缓冲），先把点亮与帧率数据拿到手。
 *   - 不使用 LVGL（ADR-0001）；只用 SDK 公开的 rt_device + rt_graphix_ops API。
 *   - 不改动任何 SDK 文件与构建脚本；新增源码放 src/ 由 src/SConscript 自动收集。
 *
 * 依据（均已查证，非猜测）：
 *   - LCD 用法：example/rt_driver/src/main.c（open("lcd") + RTGRAPHIC_CTRL_GET_INFO +
 *     set_window/draw_rect_async + tx_complete 回调）
 *   - 触摸用法：example/rt_driver/src/main.c 的 touch_read_task（drv_touch.h 的
 *     struct touch_message、rt_device_set_rx_indicate、rt_sem_take 后 rt_device_read）
 *   - 分辨率 390×450 / TE 可用 / 触摸已使能：上次编译产物的 rtconfig.h
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include "drv_touch.h"

#define M2_BAND_ROWS      60u        /* 分行刷的高度（行） */
#define M2_TARGET_MS      33u        /* 目标帧间隔：30fps */
#define M2_STAT_WINDOW    64u        /* 帧间隔统计窗口 */
#define M2_MISS_TOLERANCE 5u         /* 超过 目标+5ms 记为超时帧 */

static rt_device_t s_lcd;
static rt_device_t s_touch;
static struct rt_device_graphic_info s_lcd_info;
static uint16_t *s_band;                 /* 一行 band 缓冲（RGB565） */
static rt_uint32_t s_band_rows;          /* 实际 band 行数（可能被裁剪） */
static struct rt_semaphore s_flush_sema; /* 刷屏完成 */
static struct rt_semaphore s_tp_sema;    /* 触摸中断 */
static volatile rt_uint32_t s_tp_isr_ms; /* 触摸中断时刻（ISR 内打戳） */
static rt_uint32_t s_tp_count;

/* ---------------- 颜色工具 ---------------- */

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ---------------- LCD ---------------- */

static rt_err_t lcd_tx_done(rt_device_t dev, void *buffer)
{
    rt_sem_release(&s_flush_sema);
    return RT_EOK;
}

static int m2_lcd_init(void)
{
    s_lcd = rt_device_find("lcd");
    if (!s_lcd)
    {
        rt_kprintf("[M2] ERROR: find \"lcd\" device failed\n");
        return -1;
    }
    if (rt_device_open(s_lcd, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[M2] ERROR: open lcd failed\n");
        return -1;
    }

    memset(&s_lcd_info, 0, sizeof(s_lcd_info));
    if (rt_device_control(s_lcd, RTGRAPHIC_CTRL_GET_INFO, &s_lcd_info) != RT_EOK)
    {
        rt_kprintf("[M2] ERROR: RTGRAPHIC_CTRL_GET_INFO failed\n");
        return -1;
    }
    rt_kprintf("[M2] lcd info: width=%d height=%d bpp=%d draw_align=%d\n",
               s_lcd_info.width, s_lcd_info.height,
               s_lcd_info.bits_per_pixel, s_lcd_info.draw_align);

    rt_uint16_t fmt = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &fmt);

    rt_uint8_t brightness = 100;
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);

    rt_sem_init(&s_flush_sema, "m2flush", 0, RT_IPC_FLAG_FIFO);
    rt_device_set_tx_complete(s_lcd, lcd_tx_done);
    return 0;
}

/* 把一行颜色填进 band 缓冲 */
static void band_fill_row(uint16_t *row, rt_uint32_t width, uint16_t color)
{
    for (rt_uint32_t x = 0; x < width; x++)
        row[x] = color;
}

/* 提交一行 band（异步 + 等完成），用于统计刷屏耗时 */
static void band_flush(rt_uint32_t y0, rt_uint32_t rows)
{
    rt_graphix_ops(s_lcd)->set_window(0, y0, s_lcd_info.width - 1, y0 + rows - 1);
    rt_graphix_ops(s_lcd)->draw_rect_async((const char *)s_band, 0, y0,
                                           s_lcd_info.width - 1, y0 + rows - 1);
    rt_sem_take(&s_flush_sema, RT_WAITING_FOREVER);
}

/* 整屏刷一个颜色：分行刷，返回耗时(ms) */
static rt_uint32_t m2_fill_screen(uint16_t color)
{
    rt_uint32_t t0 = rt_tick_get_millisecond();

    for (rt_uint32_t y = 0; y < s_lcd_info.height; y += s_band_rows)
    {
        rt_uint32_t rows = s_band_rows;
        if (y + rows > s_lcd_info.height)
            rows = s_lcd_info.height - y;
        for (rt_uint32_t i = 0; i < rows; i++)
            band_fill_row(&s_band[i * s_lcd_info.width], s_lcd_info.width, color);
        band_flush(y, rows);
    }

    return rt_tick_get_millisecond() - t0;
}

/* 8 条横向色带：用于确认分辨率/像素格式/对齐是否正确 */
static rt_uint32_t m2_color_bars(void)
{
    static const uint16_t bar[8] = {0xFFFF, 0x8410, 0xF800, 0x07E0, 0x001F, 0x07FF, 0xF81F, 0xFFE0};
    rt_uint32_t t0 = rt_tick_get_millisecond();
    rt_uint32_t bar_rows = s_lcd_info.height / 8;

    for (rt_uint32_t b = 0; b < 8; b++)
    {
        rt_uint32_t y0 = b * bar_rows;
        for (rt_uint32_t y = y0; y < y0 + bar_rows; y += s_band_rows)
        {
            rt_uint32_t rows = s_band_rows;
            if (y + rows > y0 + bar_rows)
                rows = y0 + bar_rows - y;
            for (rt_uint32_t i = 0; i < rows; i++)
                band_fill_row(&s_band[i * s_lcd_info.width], s_lcd_info.width, bar[b]);
            band_flush(y, rows);
        }
    }

    return rt_tick_get_millisecond() - t0;
}

/* ---------------- 帧间隔统计 ---------------- */

static rt_uint32_t s_interval[M2_STAT_WINDOW];
static rt_uint32_t s_interval_cnt;
static rt_uint32_t s_frame_no;
static rt_uint32_t s_miss_cnt;

static int cmp_u32(const void *a, const void *b)
{
    rt_uint32_t x = *(const rt_uint32_t *)a, y = *(const rt_uint32_t *)b;
    return (x > y) - (x < y);
}

static void m2_report_stats(void)
{
    if (s_interval_cnt == 0)
        return;

    rt_uint32_t sorted[M2_STAT_WINDOW];
    rt_uint32_t n = s_interval_cnt, sum = 0, max = 0;
    memcpy(sorted, s_interval, n * sizeof(rt_uint32_t));
    for (rt_uint32_t i = 0; i < n; i++)
    {
        sum += s_interval[i];
        if (s_interval[i] > max)
            max = s_interval[i];
    }
    qsort(sorted, n, sizeof(rt_uint32_t), cmp_u32);
    rt_uint32_t p95 = sorted[(n * 95) / 100 >= n ? n - 1 : (n * 95) / 100];

    rt_kprintf("[M2] frame=%u avg=%ums p95=%ums max=%ums miss=%u (target=%ums)\n",
               s_frame_no, sum / n, p95, max, s_miss_cnt, M2_TARGET_MS);
    s_interval_cnt = 0;
    s_miss_cnt = 0;
}

/* ---------------- 帧循环线程 ---------------- */

static void m2_loop_thread(void *param)
{
    rt_uint32_t bar_y = 0;

    for (;;)
    {
        rt_uint32_t t0 = rt_tick_get_millisecond();
        uint16_t bg = rgb565(16, 16, 16);
        uint16_t fg = rgb565(0, 255, 128);

        /* 画一帧：背景 + 一条上下移动的横条（可见即证明帧在更新） */
        for (rt_uint32_t y = 0; y < s_lcd_info.height; y += s_band_rows)
        {
            rt_uint32_t rows = s_band_rows;
            if (y + rows > s_lcd_info.height)
                rows = s_lcd_info.height - y;
            for (rt_uint32_t i = 0; i < rows; i++)
            {
                int in_bar = ((y + i + s_lcd_info.height - bar_y) % s_lcd_info.height) < 12;
                band_fill_row(&s_band[i * s_lcd_info.width], s_lcd_info.width, in_bar ? fg : bg);
            }
            band_flush(y, rows);
        }

        bar_y = (bar_y + 6) % s_lcd_info.height;

        rt_uint32_t interval = rt_tick_get_millisecond() - t0;
        s_frame_no++;
        if (s_interval_cnt < M2_STAT_WINDOW)
            s_interval[s_interval_cnt++] = interval;
        if (interval > M2_TARGET_MS + M2_MISS_TOLERANCE)
            s_miss_cnt++;

        if (s_frame_no % 30 == 0)
            m2_report_stats();

        /* 保证固定帧节拍：不足则补睡 */
        rt_uint32_t used = rt_tick_get_millisecond() - t0;
        if (used < M2_TARGET_MS)
            rt_thread_delay(rt_tick_from_millisecond(M2_TARGET_MS - used));
    }
}

/* ---------------- 触摸线程 ---------------- */

static rt_err_t tp_rx_indicate(rt_device_t dev, rt_size_t size)
{
    s_tp_isr_ms = rt_tick_get_millisecond();   /* 中断内打戳，作为判定时间基准 */
    rt_sem_release(&s_tp_sema);
    return RT_EOK;
}

static void m2_touch_thread(void *param)
{
    s_touch = rt_device_find("touch");
    if (!s_touch)
    {
        rt_kprintf("[M2] ERROR: find \"touch\" device failed\n");
        return;
    }
    if (rt_device_open(s_touch, RT_DEVICE_FLAG_RDONLY) != RT_EOK)
    {
        rt_kprintf("[M2] ERROR: open touch failed\n");
        return;
    }
    rt_sem_init(&s_tp_sema, "m2tp", 0, RT_IPC_FLAG_FIFO);
    rt_device_set_rx_indicate(s_touch, tp_rx_indicate);
    rt_kprintf("[M2] touch ready\n");

    for (;;)
    {
        struct touch_message tp;

        rt_sem_take(&s_tp_sema, RT_WAITING_FOREVER);
        rt_uint32_t isr_ms = s_tp_isr_ms;
        memset(&tp, 0, sizeof(tp));
        rt_device_read(s_touch, 0, &tp, 1);
        rt_uint32_t read_ms = rt_tick_get_millisecond();

        s_tp_count++;
        rt_kprintf("[M2] touch #%u %s (%d,%d) isr=%ums read=%ums irq2read=%ums\n",
                   s_tp_count, (TOUCH_EVENT_DOWN == tp.event) ? "DOWN" : "UP",
                   tp.x, tp.y, isr_ms, read_ms, read_ms - isr_ms);
    }
}

/* ---------------- 入口 ---------------- */

void m2_probe_start(void)
{
    rt_kprintf("\n[M2] ==== M2 probe start ====\n");

    if (m2_lcd_init() != 0)
    {
        rt_kprintf("[M2] LCD init failed, probe aborted\n");
        return;
    }

    s_band_rows = M2_BAND_ROWS;
    if (s_band_rows > s_lcd_info.height)
        s_band_rows = s_lcd_info.height;
    s_band = (uint16_t *)rt_malloc(s_lcd_info.width * s_band_rows * sizeof(uint16_t));
    if (!s_band)
    {
        rt_kprintf("[M2] ERROR: band buffer malloc failed (%u B)\n",
                   (unsigned)(s_lcd_info.width * s_band_rows * 2));
        return;
    }
    rt_kprintf("[M2] band buffer %u rows, %u bytes\n",
               s_band_rows, (unsigned)(s_lcd_info.width * s_band_rows * 2));

    rt_uint32_t t;
    t = m2_fill_screen(rgb565(255, 0, 0));
    rt_kprintf("[M2] fill RED   duration=%ums\n", t);
    t = m2_fill_screen(rgb565(0, 255, 0));
    rt_kprintf("[M2] fill GREEN duration=%ums\n", t);
    t = m2_fill_screen(rgb565(0, 0, 255));
    rt_kprintf("[M2] fill BLUE  duration=%ums\n", t);
    t = m2_fill_screen(rgb565(255, 255, 255));
    rt_kprintf("[M2] fill WHITE duration=%ums\n", t);
    t = m2_color_bars();
    rt_kprintf("[M2] color bars duration=%ums\n", t);

    rt_thread_t tid = rt_thread_create("m2_loop", m2_loop_thread, RT_NULL, 2048, 20, 10);
    if (tid)
        rt_thread_startup(tid);
    tid = rt_thread_create("m2_touch", m2_touch_thread, RT_NULL, 2048, 18, 10);
    if (tid)
        rt_thread_startup(tid);

    rt_kprintf("[M2] ==== probe running ====\n");
}
