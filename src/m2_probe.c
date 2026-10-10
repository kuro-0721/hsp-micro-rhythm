/*
 * M2 关键技术验证原型 v2（临时文件：M3 会按 docs/02 拆到 fb.c / input_touch.c / game_loop.c）
 *
 * v2 的目的：判定 131ms/帧 到底是「VSYNC 限制」还是「带宽限制」。
 *   一次烧写内交替跑两种模式，每 30 帧切换一次，直接对比：
 *     mode=BAND  分行刷：每帧 8 次 set_window + draw_rect_async + 等完成回调（band=60 行）
 *     mode=FULL  整屏刷：每帧 1 次提交整屏 351KB 缓冲（放 PSRAM）
 *   两种假设的预测值差 8 倍：
 *     VSYNC 限制  → FULL 单帧 ≈16.7ms（60fps 可达）
 *     带宽限制    → FULL 单帧 ≈123ms（仍约 8fps）
 *   同时分别统计「CPU 填充耗时」与「提交等待耗时」，便于定位瓶颈在谁身上。
 *
 * 其他修复/新增：
 *   - 修 AI-09：触摸事件以前只判 DOWN、其余全当 UP，导致 MOVE(0x03) 被误报为 UP。
 *     依据 rtos/rtthread/bsp/sifli/drivers/drv_touch.h：UP=0x01 / DOWN=0x02 / MOVE=0x03。
 *   - 两种模式在提交前都做 mpu_dcache_clean()，保证对比公平（PSRAM/堆可能走 DCache）。
 *
 * 依据（已查证）：
 *   - LCD 用法：example/rt_driver/src/main.c（open("lcd") + GET_INFO + set_window +
 *     draw_rect_async + tx_complete 回调 + mpu_dcache_clean）
 *   - 触摸用法：同一例程的 touch_read_task
 *   - 分辨率 390x450 / TE 可用：上次编译产物的 rtconfig.h
 *
 * 已知局限（不得夸大）：时间戳基于 rt_tick_get_millisecond()，分辨率仅 1ms；
 *   因此「<1ms」类结论只能表述为小于一个 tick。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include "drv_touch.h"

#define M2_BAND_ROWS     60u     /* 分行刷高度（行） */
#define M2_TARGET_MS     33u     /* 目标帧间隔：30fps */
#define M2_PHASE_FRAMES  30u     /* 每 30 帧切换一次模式 */
#define M2_RING          64u     /* 统计环形缓冲长度 */

#define MODE_BAND 0
#define MODE_FULL 1
#define MODE_COUNT 2

static rt_device_t s_lcd;
static rt_device_t s_touch;
static struct rt_device_graphic_info s_lcd_info;
static uint16_t *s_band;                 /* 一行 band 的渲染缓冲（RGB565） */
static uint16_t *s_full;                 /* 整屏缓冲（RGB565，PSRAM） */
static rt_uint32_t s_band_rows;
static rt_uint32_t s_full_bytes;
static struct rt_semaphore s_flush_sema;
static struct rt_semaphore s_tp_sema;
static volatile rt_uint32_t s_tp_isr_ms;
static rt_uint32_t s_tp_count;

/* ---------------- 颜色 ---------------- */

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

/* 提交一块区域并等待完成（含 dcache 清理，保证与整屏模式口径一致） */
static void region_flush(uint16_t *buf, rt_uint32_t y0, rt_uint32_t rows, rt_uint32_t bytes)
{
    mpu_dcache_clean((uint32_t *)buf, bytes);
    rt_graphix_ops(s_lcd)->set_window(0, y0, s_lcd_info.width - 1, y0 + rows - 1);
    rt_graphix_ops(s_lcd)->draw_rect_async((const char *)buf, 0, y0,
                                           s_lcd_info.width - 1, y0 + rows - 1);
    rt_sem_take(&s_flush_sema, RT_WAITING_FOREVER);
}

/* ---------------- 两种渲染模式 ---------------- */

/* 分行刷：每帧 8 次提交；分别累计 CPU 填充与提交等待费时 */
static void render_band(uint16_t bg, uint16_t fg, rt_uint32_t bar_y,
                        rt_uint32_t *draw_ms, rt_uint32_t *flush_ms)
{
    rt_uint32_t td = 0, tf = 0;

    for (rt_uint32_t y = 0; y < s_lcd_info.height; y += s_band_rows)
    {
        rt_uint32_t rows = s_band_rows;
        if (y + rows > s_lcd_info.height)
            rows = s_lcd_info.height - y;

        rt_uint32_t t0 = rt_tick_get_millisecond();
        for (rt_uint32_t i = 0; i < rows; i++)
        {
            int in_bar = ((y + i + s_lcd_info.height - bar_y) % s_lcd_info.height) < 12;
            uint16_t *row = &s_band[i * s_lcd_info.width];
            for (rt_uint32_t x = 0; x < s_lcd_info.width; x++)
                row[x] = in_bar ? fg : bg;
        }
        rt_uint32_t t1 = rt_tick_get_millisecond();

        region_flush(s_band, y, rows, s_lcd_info.width * rows * sizeof(uint16_t));

        rt_uint32_t t2 = rt_tick_get_millisecond();
        td += t1 - t0;
        tf += t2 - t1;
    }

    *draw_ms = td;
    *flush_ms = tf;
}

/* 整屏刷：每帧只提交一次 */
static void render_full(uint16_t bg, uint16_t fg, rt_uint32_t bar_y,
                        rt_uint32_t *draw_ms, rt_uint32_t *flush_ms)
{
    rt_uint32_t t0 = rt_tick_get_millisecond();

    for (rt_uint32_t y = 0; y < s_lcd_info.height; y++)
    {
        int in_bar = ((y + s_lcd_info.height - bar_y) % s_lcd_info.height) < 12;
        uint16_t *row = &s_full[y * s_lcd_info.width];
        for (rt_uint32_t x = 0; x < s_lcd_info.width; x++)
            row[x] = in_bar ? fg : bg;
    }

    rt_uint32_t t1 = rt_tick_get_millisecond();
    region_flush(s_full, 0, s_lcd_info.height, s_full_bytes);
    rt_uint32_t t2 = rt_tick_get_millisecond();

    *draw_ms = t1 - t0;
    *flush_ms = t2 - t1;
}

/* ---------------- 启动诊断：整屏填充与色带（分行刷） ---------------- */

static rt_uint32_t m2_fill_screen(uint16_t color)
{
    rt_uint32_t t0 = rt_tick_get_millisecond();

    for (rt_uint32_t y = 0; y < s_lcd_info.height; y += s_band_rows)
    {
        rt_uint32_t rows = s_band_rows;
        if (y + rows > s_lcd_info.height)
            rows = s_lcd_info.height - y;
        for (rt_uint32_t i = 0; i < rows; i++)
        {
            uint16_t *row = &s_band[i * s_lcd_info.width];
            for (rt_uint32_t x = 0; x < s_lcd_info.width; x++)
                row[x] = color;
        }
        region_flush(s_band, y, rows, s_lcd_info.width * rows * sizeof(uint16_t));
    }

    return rt_tick_get_millisecond() - t0;
}

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
            {
                uint16_t *row = &s_band[i * s_lcd_info.width];
                for (rt_uint32_t x = 0; x < s_lcd_info.width; x++)
                    row[x] = bar[b];
            }
            region_flush(s_band, y, rows, s_lcd_info.width * rows * sizeof(uint16_t));
        }
    }

    return rt_tick_get_millisecond() - t0;
}

/* ---------------- 统计 ---------------- */

struct m2_stats
{
    rt_uint32_t n;
    rt_uint32_t sum_total, max_total;
    rt_uint32_t sum_draw, sum_flush;
    rt_uint32_t miss;
    rt_uint32_t ring[M2_RING];
};

static struct m2_stats s_st[MODE_COUNT];
static rt_uint32_t s_frame_no;

static int cmp_u32(const void *a, const void *b)
{
    rt_uint32_t x = *(const rt_uint32_t *)a, y = *(const rt_uint32_t *)b;
    return (x > y) - (x < y);
}

static void stats_add(int mode, rt_uint32_t draw_ms, rt_uint32_t flush_ms)
{
    struct m2_stats *s = &s_st[mode];
    rt_uint32_t total = draw_ms + flush_ms;

    s->sum_draw += draw_ms;
    s->sum_flush += flush_ms;
    s->sum_total += total;
    if (total > s->max_total)
        s->max_total = total;
    if (total > M2_TARGET_MS + 5u)
        s->miss++;
    if (s->n < M2_RING)
        s->ring[s->n] = total;
    s->n++;
}

static rt_uint32_t stats_p95(const struct m2_stats *s)
{
    rt_uint32_t sorted[M2_RING];
    rt_uint32_t n = (s->n < M2_RING) ? s->n : M2_RING;
    rt_uint32_t idx;

    if (n == 0)
        return 0;
    memcpy(sorted, s->ring, n * sizeof(rt_uint32_t));
    qsort(sorted, n, sizeof(rt_uint32_t), cmp_u32);
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

static void stats_report(void)
{
    for (int m = 0; m < MODE_COUNT; m++)
    {
        struct m2_stats *s = &s_st[m];
        const char *name = (m == MODE_BAND) ? "BAND" : "FULL";
        rt_uint32_t n = s->n ? s->n : 1;

        if (s->n == 0)
        {
            rt_kprintf("[M2] mode=%s (no sample)\n", name);
            continue;
        }
        rt_kprintf("[M2] mode=%s n=%u avg=%ums p95=%ums max=%ums draw=%ums flush=%ums miss=%u\n",
                   name, s->n, s->sum_total / n, stats_p95(s), s->max_total,
                   s->sum_draw / n, s->sum_flush / n, s->miss);
    }
    rt_kprintf("[M2] ---- frame=%u target=%ums ----\n", s_frame_no, M2_TARGET_MS);
}

/* ---------------- 帧循环线程 ---------------- */

static void m2_loop_thread(void *param)
{
    rt_uint32_t bar_y = 0;
    uint16_t bg = rgb565(16, 16, 16);
    uint16_t fg = rgb565(0, 255, 128);
    int mode = MODE_BAND;

    for (;;)
    {
        rt_uint32_t draw_ms = 0, flush_ms = 0;

        if (mode == MODE_BAND || !s_full)
            render_band(bg, fg, bar_y, &draw_ms, &flush_ms);
        else
            render_full(bg, fg, bar_y, &draw_ms, &flush_ms);

        bar_y = (bar_y + 6) % s_lcd_info.height;
        stats_add(mode, draw_ms, flush_ms);
        s_frame_no++;

        if (s_frame_no % M2_PHASE_FRAMES == 0)
        {
            stats_report();
            mode = (mode == MODE_BAND) ? MODE_FULL : MODE_BAND;
            rt_kprintf("[M2] switch to mode=%s\n", (mode == MODE_BAND) ? "BAND" : "FULL");
        }

        rt_uint32_t used = draw_ms + flush_ms;
        if (used < M2_TARGET_MS)
            rt_thread_delay(rt_tick_from_millisecond(M2_TARGET_MS - used));
    }
}

/* ---------------- 触摸线程 ---------------- */

static rt_err_t tp_rx_indicate(rt_device_t dev, rt_size_t size)
{
    s_tp_isr_ms = rt_tick_get_millisecond();   /* 中断内打戳（分辨率 1ms） */
    rt_sem_release(&s_tp_sema);
    return RT_EOK;
}

/* 事件分类。修复 AI-09：以前只判 DOWN、其余全当 UP，MOVE 被误报为 UP。
 * 依据 rtos/rtthread/bsp/sifli/drivers/drv_touch.h：
 *   TOUCH_EVENT_UP=0x01  TOUCH_EVENT_DOWN=0x02  TOUCH_EVENT_MOVE=0x03 */
static const char *tp_event_name(rt_uint8_t ev)
{
    switch (ev)
    {
    case TOUCH_EVENT_DOWN: return "DOWN";
    case TOUCH_EVENT_MOVE: return "MOVE";
    case TOUCH_EVENT_UP:   return "UP";
    default:               return "OTHER";
    }
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
        rt_kprintf("[M2] touch #%u %s(0x%02x) (%d,%d) isr=%ums read=%ums irq2read=%ums\n",
                   s_tp_count, tp_event_name(tp.event), tp.event,
                   tp.x, tp.y, isr_ms, read_ms, read_ms - isr_ms);
    }
}

/* ---------------- 入口 ---------------- */

void m2_probe_start(void)
{
    rt_kprintf("\n[M2] ==== M2 probe v2 start ====\n");

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
    rt_kprintf("[M2] band buffer %u rows, %u bytes @0x%08x\n",
               s_band_rows, (unsigned)(s_lcd_info.width * s_band_rows * 2), (unsigned)(rt_ubase_t)s_band);

    s_full_bytes = s_lcd_info.width * s_lcd_info.height * sizeof(uint16_t);
    s_full = (uint16_t *)rt_malloc(s_full_bytes);
    if (!s_full)
        rt_kprintf("[M2] WARN: full-screen buffer malloc failed (%u B) -> FULL mode disabled\n",
                   (unsigned)s_full_bytes);
    else
        rt_kprintf("[M2] full buffer %u bytes @0x%08x (FULL mode enabled)\n",
                   (unsigned)s_full_bytes, (unsigned)(rt_ubase_t)s_full);

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

    /* 保持色带 15 秒，供上板拍照取证（M2 证据缺口：8 色带照片）；
     * 拍照完成后板子会自动继续进入帧率对比实验。 */
    rt_kprintf("[M2] >>> HOLD 15s for photo evidence - photograph the screen NOW <<<\n");
    for (int hold = 15; hold > 0; hold--)
    {
        rt_kprintf("[M2] hold %d\n", hold);
        rt_thread_mdelay(1000);
    }

    rt_thread_t tid = rt_thread_create("m2_loop", m2_loop_thread, RT_NULL, 2048, 20, 10);
    if (tid)
        rt_thread_startup(tid);
    tid = rt_thread_create("m2_touch", m2_touch_thread, RT_NULL, 2048, 18, 10);
    if (tid)
        rt_thread_startup(tid);

    rt_kprintf("[M2] ==== probe v2 running ====\n");
}
