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
#define COL_HUD_BG  RGB565(40, 40, 40)      /* HUD 底板：比背景亮一档，把文字与音符分开 */

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

/*
 * 5×7 点阵文字。scale 为整数倍缩放（1..4）。
 * 字符集 0x20-0x5A（空格、数字、标点、大写字母）；小写自动转大写，其余字符显示为空格。
 * 只写背缓冲，不提交；超出屏幕的像素自动裁剪。
 */
void gfx_text(int x, int y, const char *s, uint16_t color, int scale);

/* 计算 gfx_text() 画出的像素宽度（不含右侧字距）。空串返回 0。 */
int  gfx_text_width(const char *s, int scale);

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

/*
 * 把一个**合成**点击注入触摸事件队列（仅供 src/autoplay.c 的自动演奏使用）。
 * 下游路径与真实触摸完全一致，但不经过 FT6146 中断与 I2C 读点，
 * 所以用本接口测出的延迟**不能**当 TC-104 的端到端延迟。
 */
int touch_inject(uint32_t t_ms, uint16_t x, uint16_t y);

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

/*
 * 判定延迟直方图（REQ-003 的验收口径 / REQ-007 第 2 节）。
 * 这里统计的是「触摸中断时刻 → 判定执行时刻」的延迟，**不是**「触摸→画面翻转」。
 * 由 scene 在每次成功判定后调用 judge_latency_add()。
 */
#define JUDGE_LAT_BUCKETS   6       /* 0-10 / 11-20 / 21-33 / 34-50 / 51-100 / >100 ms */

void     judge_latency_add(uint32_t lag_ms);
void     judge_latency_reset(void);
void     judge_latency_hist(uint32_t *out, int n_buckets);  /* 不足的桶补 0 */
uint32_t judge_latency_count(void);
uint32_t judge_latency_p95_ms(void);                        /* 无样本时返回 0 */

/* ---------------- chart：文本谱面（src/chart.c，REQ-006） ---------------- */

struct chart_info
{
    uint32_t notes;         /* 音符数 */
    uint32_t first_ms;      /* 首个音符时刻（无音符时为 0） */
    uint32_t last_ms;       /* 末个音符时刻 */
    uint32_t per_lane[4];   /* 各轨音符数 */
};

/*
 * 解析谱面文本（格式见 docs/02 §4.2）。
 * 返回：>0 = 音符数；0 = 语法合法但无音符；负 = -(出错行号)，行号从 1 起算。
 * 注意：解析成功会覆盖内部音符池；失败时请把它当作“当前谱面无效”。
 */
int  chart_parse(const char *text, int len);   /* len < 0 表示按 '\0' 结尾处理 */

/* 解析内置谱面并装入判定引擎。返回音符数（>0）或负错误码。 */
int  chart_load_builtin(void);

/* 当前已解析的谱面（供 scene 渲染用）。未加载时 chart_note_count() 为 0。 */
const struct note *chart_notes(void);
uint32_t chart_note_count(void);

/* 谱面摘要（音符数 / 首末时刻 / 轨道分布），供串口打印与第 4 步自检。 */
const struct chart_info *chart_summary(void);

/* ---------------- scene：场景状态机（src/scene.c，REQ-001/003/004/005） ---------------- */

enum scene_id
{
    SCENE_TITLE = 0,    /* 主界面：显示曲名与「开始」提示 */
    SCENE_PLAYING,      /* 演奏中 */
    SCENE_RESULT,       /* 结算页 */
};

/* 重置到 TITLE（上电即主界面，REQ-001）。 */
void scene_init(void);

/* 每帧调用一次（帧循环线程）：出队触摸事件并分派、推进判定、判是否进结算。 */
void scene_update(uint32_t now_ms);

/* 每帧调用一次：只写背缓冲，不提交（提交由 game_loop 统一做）。 */
void scene_render(void);

enum scene_id scene_current(void);

/* 当前谱面时间（毫秒，只在 PLAYING 有意义）；供自动演奏对齐节点使用。 */
uint32_t scene_chart_ms(void);

/* ---------------- autoplay：自动演奏（测试用，src/autoplay.c） ---------------- */

/*
 * 启动自动演奏线程：按谱面节点自动注入点击，用于 TC-101（连续演奏 30s）
 * 与 TC-103（连续演奏 10min）的负载。AUTOPLAY_ENABLE=0 时本函数为空。
 */
int autoplay_start(void);

/* ---------------- game_loop：固定帧循环与统计 ---------------- */

struct loop_stat
{
    uint32_t frames;        /* 累计帧数 */
    uint32_t avg_ms;        /* 平均单帧处理耗时（draw + flush） */
    uint32_t p95_ms;        /* 95 分位单帧处理耗时 */
    uint32_t max_ms;        /* 最长单帧处理耗时 */
    uint32_t avg_draw_ms;   /* 平均 CPU 绘制耗时 */
    uint32_t avg_flush_ms;  /* 平均提交等待耗时 */
    uint32_t miss;          /* 掉帧数（帧间隔超过 FRAME_MS + 容差） */
    uint32_t target_ms;     /* 目标帧间隔 */

    /*
     * 以下是**真正的帧间隔**（本帧开始减上帧开始），才是 REQ-101 说的「帧间隔」。
     * 上面的 avg_ms/p95_ms 只是处理耗时：flush 只需 26ms 时帧循环会用
     * rt_thread_delay 补足到 33ms，两者会差出一个可观的量。
     * 首帧没有「上一帧」，不计入这三项（见 docs/04 的 L-11）。
     */
    uint32_t avg_interval_ms;
    uint32_t p95_interval_ms;
    uint32_t max_interval_ms;
};

/* 初始化（依赖 fb_init 已成功）并启动帧循环线程。 */
int  game_loop_start(void);

/* 读取统计快照。 */
void loop_stats(struct loop_stat *out);

/* ---------------- sfx：命中音效（src/sfx.c，REQ-009 / ADR-0005） ---------------- */

/*
 * 音效是**代码合成**的（440Hz 方波 + 一阶低通 + 指数衰减），不依赖任何音频文件
 * 或 TF 卡，因此 P0/P1 阶段都能用，也天然满足 REQ-105 的素材合规。
 * 播放走 audio_server（需 CONFIG_AUDIO_USING_MANAGER=y + AUDIO_PATH_USING_HCPU=y）。
 * 数据由 audio_server 的缓存半空回调驱动：callback 里持续补数据（音效或静音），
 * 不是独立线程主动写 —— 详见 src/sfx.c 文件头。
 *
 * 合成、打开音频 client。成功返回 0。
 * 失败不会影响游戏逻辑，只是命中时没有声音（调用方应打 WARN 继续）。
 */
int  sfx_init(void);

/* 开关：关闭后 sfx_trigger() 只记时间戳、不发声（REQ-009 的「可开关」）。 */
void sfx_set_enabled(int on);
int  sfx_enabled(void);

/*
 * 触发一次命中音效，返回调用时刻（ms）供 REQ-009 的客观判据使用。
 * **本函数不阻塞**：只重置播放位置并置标志，实际的 audio_write 发生在音频回调里，
 * 因此可以安全地在判定路径里调用。
 */
uint32_t sfx_trigger(void);

/* 最近一次触发的时刻（ms），无触发时返回 0。 */
uint32_t sfx_last_trigger_ms(void);

/*
 * 最近一次触发中「音效第一个样本进入音频缓存」与 trigger 时刻的差（ms）。
 * 这是比 sfx_trigger() 本身更接近真实发声的度量，但仍不含 DAC 与喇叭的时延。
 */
uint32_t sfx_feed_latency_ms(void);

/* 诊断：回调次数 / 写入成功次数 / 最近一次 audio_write 返回值（0 = 成功）。 */
void sfx_get_diag(uint32_t *cb_count, uint32_t *feed_ok, int *last_write_ret);

/* ---------------- audio：内存 MP3 与音频时间基准（src/audio.c，REQ-008 / CR-05） ---------------- */
/*
 * 谱面时间轴以**音频播放进度**为基准，而不是进入 PLAYING 时的系统毫秒。
 * 进度来自 audio_register_10ms_tx_dma_callback()：音频 TX DMA 每 10ms 回调一次，
 * 回调次数 × 10 = 已真正被 DAC 取走的毫秒数。**分辨率为 10ms**，不是 1ms。
 *
 * 曲目以 C 数组形式编入固件（src/mp3_sample.c），用 mp3ctrl_open_buffer() 从内存解码，
 * **不使用 TF 卡** —— 原因见 CR-05：启用 SPI_MSD 会在 main() 之前自动做卡握手，
 * 卡不响应会直接把整个系统阻塞在启动阶段（屏幕完全不亮，已实测）。
 */
int  audio_init(void);

/* 播放固件内置曲目。成功返回 0。 */
int  audio_play(void);

/* 当前播放进度（ms），10ms 粒度；未开始播放时返回 0。 */
uint32_t audio_pos_ms(void);

/* 停止播放并释放解码器。 */
void audio_stop(void);

int  audio_is_playing(void);
int  audio_finished(void);
int  audio_is_mounted(void);

/*
 * 阶段 1 自测：播放 TF 卡根目录的 /test_beat_120bpm.mp3，每 500ms 打印一次进度。
 * 只用于验证「挂载 → 播放 → 进度」链路，接入谱面同步后关闭。
 */
void audio_boot_test(void);

/*
 * 异步启动 BGM 播放（REQ-008 场景接入）。
 * mp3ctrl_play() 是阻塞的，**不能在帧循环里直接调 audio_play()**，
 * 这里用短命线程去播，创建成功返回 0。
 */
int  audio_play_async(void);

/*
 * 异步启动 TF 卡初始化（独立线程，优先级 22）。
 *
 * **必须用这个而不是直接调 audio_init()**：msd_init 是阻塞的卡握手，卡不通时会
 * 永久占住 spi1 互斥量。2026-10-31 实测，同步调用会让 main 线程挂起、整个系统停摆。
 * 异步后最坏只是音频不可用，游戏照常运行。
 */
int  audio_start_async(void);

/* ---------------- selftest：启动自检（src/selftest.c，REQ-007） ---------------- */
#define SELFTEST_FRAME_WINDOW   150u    /* 约 5s（150 帧 × 33ms），第 3 节就绪的帧数 */
#define SELFTEST_P95_LIMIT_MS   34u     /* REQ-101 判据（CR-04 放宽后的值） */

/* 第 1~2 节（谱面 + 判定）：启动时调用一次，返回失败的节数。 */
int  selftest_startup(void);

/* 第 3 节（帧率）+ 总结论：由帧循环在累计帧数达到 SELFTEST_FRAME_WINDOW 后调一次。 */
void selftest_frame_stats(void);

/* 判定延迟直方图的实测报告：由 scene 在进入 RESULT 时调用（无样本则不打印）。 */
void selftest_latency_report(void);

/* ---------------- 应用入口 ---------------- */

/* 由 main.c 调用：初始化 LCD → 启动帧循环。成功返回 0。 */
int app_start(void);

#endif /* APP_H */
