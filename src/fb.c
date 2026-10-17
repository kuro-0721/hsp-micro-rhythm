/*
 * fb.c —— 整屏帧缓冲 + 每帧单次提交
 *
 * 依据 ADR-0003：M2 实测「分行刷（每帧 8 次提交）132ms/帧」vs「整屏单次提交 29ms/帧、
 * p95 31ms、0 超时帧」，瓶颈是每次提交等一个 VSYNC（≈15.8ms），不是带宽。
 * 因此这里采用**单缓冲 + 每帧一次全屏提交**；双缓冲留作后续扩展（docs/02 §4.1）。
 *
 * LCD 用法逐字沿用 SDK 例程 example/rt_driver/src/main.c 中已上板验证过的写法：
 *   rt_device_find("lcd") → open → RTGRAPHIC_CTRL_GET_INFO → SET_BUF_FORMAT(RGB565)
 *   → SET_BRIGHTNESS → rt_device_set_tx_complete(回调)
 *   提交：mpu_dcache_clean → set_window → draw_rect_async → 等 tx_complete 回调
 *
 * 缓冲大小：390x450x2 = 351,000 字节，由 RT-Thread 堆分配（本板堆在 PSRAM，8MB 余量充足）。
 */
#include "app.h"
#include <string.h>

static rt_device_t s_lcd;
static struct rt_device_graphic_info s_info;
static uint16_t *s_buf;
static rt_uint32_t s_buf_bytes;
static struct rt_semaphore s_flush_sema;
static int s_ready;
static int s_rect_warned;

static rt_err_t lcd_tx_done(rt_device_t dev, void *buffer)
{
    rt_sem_release(&s_flush_sema);
    return RT_EOK;
}

int fb_init(void)
{
    if (s_ready)
        return 0;

    s_lcd = rt_device_find("lcd");
    if (!s_lcd)
    {
        rt_kprintf("[FB] ERROR: device \"lcd\" not found\n");
        return -1;
    }
    if (rt_device_open(s_lcd, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[FB] ERROR: open lcd failed\n");
        return -1;
    }

    memset(&s_info, 0, sizeof(s_info));
    if (rt_device_control(s_lcd, RTGRAPHIC_CTRL_GET_INFO, &s_info) != RT_EOK)
    {
        rt_kprintf("[FB] ERROR: RTGRAPHIC_CTRL_GET_INFO failed\n");
        return -1;
    }

    rt_uint16_t fmt = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &fmt);

    rt_uint8_t brightness = 100;
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);

    s_buf_bytes = (rt_uint32_t)s_info.width * s_info.height * sizeof(uint16_t);
    s_buf = (uint16_t *)rt_malloc(s_buf_bytes);
    if (!s_buf)
    {
        rt_kprintf("[FB] ERROR: malloc %u bytes for framebuffer failed\n",
                   (unsigned)s_buf_bytes);
        return -1;
    }
    memset(s_buf, 0, s_buf_bytes);

    rt_sem_init(&s_flush_sema, "fbf", 0, RT_IPC_FLAG_FIFO);
    rt_device_set_tx_complete(s_lcd, lcd_tx_done);

    s_ready = 1;
    rt_kprintf("[FB] lcd %ux%u bpp=%u, framebuffer %u bytes @0x%08x\n",
               s_info.width, s_info.height, s_info.bits_per_pixel,
               (unsigned)s_buf_bytes, (unsigned)(rt_ubase_t)s_buf);
    return 0;
}

uint16_t *fb_backbuffer(void)
{
    return s_buf;
}

int fb_width(void)
{
    return (int)s_info.width;
}

int fb_height(void)
{
    return (int)s_info.height;
}

void fb_clear(uint16_t color)
{
    if (!s_buf)
        return;

    rt_uint32_t n = (rt_uint32_t)s_info.width * s_info.height;
    uint16_t *p = s_buf;

    /* 按 32 位写加速：一次写两个像素 */
    rt_uint32_t pair = ((rt_uint32_t)color << 16) | color;
    rt_uint32_t *p32 = (rt_uint32_t *)p;
    rt_uint32_t n32 = n >> 1;

    for (rt_uint32_t i = 0; i < n32; i++)
        p32[i] = pair;
    if (n & 1u)
        p[n - 1] = color;
}

int fb_present(void)
{
    if (!s_ready)
        return -1;

    mpu_dcache_clean((uint32_t *)s_buf, s_buf_bytes);
    rt_graphix_ops(s_lcd)->set_window(0, 0, s_info.width - 1, s_info.height - 1);
    rt_graphix_ops(s_lcd)->draw_rect_async((const char *)s_buf, 0, 0,
                                          s_info.width - 1, s_info.height - 1);
    rt_sem_take(&s_flush_sema, RT_WAITING_FOREVER);
    return 0;
}

/*
 * 脏区提交（预留接口）。
 *
 * 限制：set_window + draw_rect_async 要求缓冲在内存中**连续**，而只有「整宽」区域
 * 在整屏缓冲里才是连续的。所以本接口只接受 x0=0 且 x1=width-1 的区域；其它形状
 * 会退回整屏提交（并只警告一次），避免出现「看起来只提交了一块、实际刷了 8 行 × N 次
 * VSYNC」那种把性能吃掉的隐性行为。
 */
int fb_flush_rect(int x0, int y0, int x1, int y1)
{
    if (!s_ready)
        return -1;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int)s_info.width - 1)
        x1 = (int)s_info.width - 1;
    if (y1 > (int)s_info.height - 1)
        y1 = (int)s_info.height - 1;
    if (x0 > x1 || y0 > y1)
        return -1;

    if (x0 != 0 || x1 != (int)s_info.width - 1)
    {
        if (!s_rect_warned)
        {
            s_rect_warned = 1;
            rt_kprintf("[FB] WARN: fb_flush_rect only supports full-width bands, "
                       "falling back to full-screen present\n");
        }
        return fb_present();
    }

    rt_uint32_t w = s_info.width;
    uint16_t *p = s_buf + (rt_uint32_t)y0 * w;
    rt_uint32_t bytes = (rt_uint32_t)(y1 - y0 + 1) * w * sizeof(uint16_t);

    mpu_dcache_clean((uint32_t *)p, bytes);
    rt_graphix_ops(s_lcd)->set_window(0, y0, w - 1, y1);
    rt_graphix_ops(s_lcd)->draw_rect_async((const char *)p, 0, y0, w - 1, y1);
    rt_sem_take(&s_flush_sema, RT_WAITING_FOREVER);
    return 0;
}
