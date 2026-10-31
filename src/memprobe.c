/*
 * memprobe.c —— 堆内存探针（临时诊断工具，REQ-008/REQ-009 混音可行性评估用）
 *
 * 为什么需要它：
 *   2026-10-31 尝试开启 SDK 软件混音（CONFIG_SOFTWARE_TX_MIX_ENABLE）后，
 *   上电几秒就断言：
 *     Assertion failed at function:audio_client_init, line:3834 ,(handle->ring_pool)
 *     total memory: 435388  used memory: 433244
 *   日志只能看到"最后时刻"的 2 个数字，看不出内存是在哪一步被吃掉的。
 *   本探针在关键节点各打一行，用实测数据代替猜测。
 *
 * 本板内存事实（从 build_.../main.map 算出，别再用"堆在 PSRAM"那个错误前提）：
 *   __bss_end   = 0x20015724
 *   HEAP_END    = 0x2007fc20
 *   堆总大小     = 435,388 字节，全部在 SRAM
 *   其中帧缓冲   = 351,000 字节（fb.c 用 rt_malloc 从同一个堆拿）
 *   → 其余所有线程/驱动一共只剩约 84KB
 *
 * 用完即可删除本文件（src/SConscript 自动收集 *.c，删掉就自动不再编译）。
 */
#include "app.h"

void memprobe(const char *tag)
{
    rt_uint32_t total = 0;
    rt_uint32_t used = 0;
    rt_uint32_t max_used = 0;

    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("[MEM] %-20s total=%u used=%u free=%u max=%u\n",
               tag ? tag : "?",
               (unsigned)total,
               (unsigned)used,
               (unsigned)(total - used),
               (unsigned)max_used);
}
