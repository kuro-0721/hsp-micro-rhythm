/*
 * input_touch.c —— 触摸输入：中断打戳 + 读点 + 事件队列
 *
 * 对应 docs/02 §3 的 input_touch 模块与 §4.1 的 touch_pop_event()。
 *
 * 设计要点（REQ-104：判定的时间精度不能被 I2C 读点耗时污染）：
 *   1. 中断回调（rx_indicate）里**立刻**记 rt_tick_get_millisecond()，这是判定用的时间戳；
 *   2. 读点（rt_device_read，走 I2C）在线程里做，M2 实测中断→读点 < 1ms；
 *   3. P0 只做「点击判定」，因此只把 TOUCH_EVENT_DOWN 入队，MOVE/UP 丢弃
 *      （M2 时踩过「只判 DOWN、其余全当 UP」的坑，见 docs/06 AI-09，这里按枚举精确分类）。
 *
 * 用法（见 docs/02 §4.1）：
 *   struct touch_ev ev;
 *   while (touch_pop_event(&ev) == 0) { ... ev.t_ms / ev.x / ev.y ... }
 */
#include "app.h"
#include <string.h>
#include "drv_touch.h"

static rt_device_t s_touch;
static struct rt_semaphore s_sema;
static volatile uint32_t s_isr_ms;
static volatile uint32_t s_dropped;
static volatile int s_pressed;              /* 当前是否已处于「按下」状态（去抖用） */
static volatile uint32_t s_last_down_ms;

static struct touch_ev s_queue[TP_QUEUE_LEN];
static volatile int s_head;     /* 生产者（读点线程）写 */
static volatile int s_tail;     /* 消费者（游戏逻辑）写 */

static rt_err_t tp_rx_indicate(rt_device_t dev, rt_size_t size)
{
    (void)dev;
    (void)size;
    /* 中断里只做最小动作：打时间戳 + 唤醒线程 */
    s_isr_ms = rt_tick_get_millisecond();
    rt_sem_release(&s_sema);
    return RT_EOK;
}

static void tp_push(uint32_t t_ms, uint16_t x, uint16_t y)
{
    int next = (s_head + 1) % TP_QUEUE_LEN;

    if (next == s_tail)
    {
        /* 队列满：丢弃最新事件并计数（不覆盖旧事件，保证时间顺序） */
        s_dropped++;
        return;
    }

    s_queue[s_head].t_ms = t_ms;
    s_queue[s_head].x = x;
    s_queue[s_head].y = y;
    s_head = next;
}

int touch_pop_event(struct touch_ev *out)
{
    if (!out)
        return -1;

    rt_enter_critical();
    if (s_tail == s_head)
    {
        rt_exit_critical();
        return -1;
    }
    *out = s_queue[s_tail];
    s_tail = (s_tail + 1) % TP_QUEUE_LEN;
    rt_exit_critical();
    return 0;
}

static void tp_thread(void *param)
{
    (void)param;

    for (;;)
    {
        struct touch_message msg;
        uint32_t isr_ms;

        if (rt_sem_take(&s_sema, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        isr_ms = s_isr_ms;

        memset(&msg, 0, sizeof(msg));
        if (rt_device_read(s_touch, 0, &msg, 1) != 1)
            continue;

        /*
         * 事件处理：一次「按下」只产生一个事件（去抖）。
         *
         * 实测（2026-10-17）：手指在屏上拖动时驱动会**持续报 DOWN**（间隔 ~11ms、坐标渐变），
         * 若按「DOWN 就入队」会把一次拖动拆成几十次「点击」，直接污染判定。
         * 所以维护 s_pressed：DOWN 仅在「尚未按下」时入队，UP 才清除状态；
         * 另加 1s 兑底，防止 UP 事件丢失导致状态卡死。
         */
        switch (msg.event)
        {
        case TOUCH_EVENT_DOWN:
            if (!s_pressed || (isr_ms - s_last_down_ms) > 1000u)
            {
                s_pressed = 1;
                s_last_down_ms = isr_ms;
                tp_push(isr_ms, msg.x, msg.y);
            }
            break;

        case TOUCH_EVENT_UP:
            s_pressed = 0;
            break;

        default:
            /* TOUCH_EVENT_MOVE 等：按下期间一律忽略 */
            break;
        }
    }
}

int touch_init(void)
{
    rt_thread_t tid;
    int i;

    s_head = 0;
    s_tail = 0;
    s_dropped = 0;
    s_pressed = 0;
    s_last_down_ms = 0;
    for (i = 0; i < TP_QUEUE_LEN; i++)
    {
        s_queue[i].t_ms = 0;
        s_queue[i].x = 0;
        s_queue[i].y = 0;
    }

    s_touch = rt_device_find("touch");
    if (!s_touch)
    {
        rt_kprintf("[TP] ERROR: device \"touch\" not found\n");
        return -1;
    }
    if (rt_device_open(s_touch, RT_DEVICE_FLAG_RDONLY) != RT_EOK)
    {
        rt_kprintf("[TP] ERROR: open touch failed\n");
        return -1;
    }

    rt_sem_init(&s_sema, "tp", 0, RT_IPC_FLAG_FIFO);
    rt_device_set_rx_indicate(s_touch, tp_rx_indicate);

    tid = rt_thread_create("tp", tp_thread, RT_NULL, 2048, 18, 10);
    if (!tid)
    {
        rt_kprintf("[TP] ERROR: create touch thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);

    rt_kprintf("[TP] touch ready (queue=%d, only DOWN events are queued)\n",
               TP_QUEUE_LEN);
    return 0;
}
