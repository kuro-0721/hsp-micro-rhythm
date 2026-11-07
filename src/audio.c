/*
 * audio.c —— TF 卡 MP3 播放与「音频进度」时间基准（REQ-008 / TC-008）
 *
 * 【为什么需要它】
 *   P0 阶段谱面时间轴用系统毫秒计时（进入 PLAYING 那一刻起算）。音频一旦变成
 *   播放 MP3，「进入 PLAYING 的时刻」与「第一个音符真正从喇叭出来的时刻」之间
 *   会多出解码启动、缓存填充等不确定延迟，直接用系统毫秒会导致音画错位。
 *   因此 REQ-008 要求时间轴以**音频播放进度**为基准。
 *
 * 【进度为什么用 DMA 回调数，而不是查解码器】
 *   audio_mp3ctrl.h 里 mp3ctrl 没有 ms 级进度查询接口：
 *     - MP3CTRL_IOCTRL_* 只有 LOOP / CHANGE_FILE / THREAD_PRIORITY / FADE_OUT
 *     - callback 的 as_callback_cmd_user + 0 只给「当前播放时间（秒）」，精度不够
 *   最终采用 audio_server 提供的 audio_register_10ms_tx_dma_callback()：
 *   音频 TX DMA 每 10ms 回调一次，回调次数 × 10 就是「已经真正被 DAC 取走」的毫秒数。
 *   这比数解码样本更准 —— 解码样本包含还在缓存里没播出的部分。
 *
 * 【精度与局限】
 *   分辨率 10ms（受限于 10ms DMA 回调粒度），满足 REQ-008 的 ≤50ms 判据，
 *   但报告里必须写清这是 10ms 粒度，不是精确到 1ms。
 *
 * 【硬件依据】
 *   板级 bsp_pinmux.c 第 175-181 行无条件把 SPI1 配为 TF 卡
 *   （PA24 DIO / PA25 DI / PA28 CLK / PA29 CS，PA27 卡检测），
 *   与 Nor Flash 的 MPI1（PAD_SA01~SA12）互不冲突。
 */
#include "app.h"
#include "audio_server.h"
#include "audio_mp3ctrl.h"

/*
 * 音频进度时间基准的换算参数（2026-10-31 修正，之前算错了 2.755 倍）
 *
 * audio_register_10ms_tx_dma_callback() 这个名字容易要命的人误以为周期就是 10ms，
 * **实际周期 = 一次 DMA 装多少样本 / 设备采样率**。
 * 一次 DMA 的字节数由 audio_server_internal.h:120-124 的 TX_DMA_SIZE 决定：
 *
 *   #if defined(SOFTWARE_TX_MIX_ENABLE) || defined(AUDIO_RX_USING_I2S) || ...
 *       #define TX_DMA_SIZE  (CODEC_DATA_UNIT_LEN)        // 320 字节 = 160 样本
 *   #else
 *       #define TX_DMA_SIZE  (CODEC_DATA_UNIT_LEN * 3)    // 960 字节 = 480 样本
 *   #endif
 *
 * 所以这个值必须跟着编译开关走，不能写死。
 *
 * 实测验证（evidence/M4/mix_run2_44k1_2026-10-31.log）：
 *   8.05 秒的测试曲目被算成 22180ms → 比例 22180/8050 = 2.755
 *   而 160 样本 @44100Hz = 3.628ms，按 10ms 累加会放大 10/3.628 = 2.756 倍 —— 完全对上。
 */
#if defined(SOFTWARE_TX_MIX_ENABLE)
    #define AUDIO_DMA_SAMPLES   160u    /* TX_DMA_SIZE=320 字节 / 2 */
#else
    #define AUDIO_DMA_SAMPLES   480u    /* TX_DMA_SIZE=960 字节 / 2 */
#endif

/*
 * 设备实际播放采样率 = 第一个打开的 client 的 write_samplerate
 * （audio_server.c:2497 `tx_mix_dst_samplerate = client->parameter.write_samplerate`）。
 * 本工程 sfx_init() 先于 mp3 打开，sfx.c 的 SFX_SAMPLE_RATE 就是 44100，
 * 所以设备跑 44100。两边必须一致，否则进度又会算错。
 * 2026-10-31 特意把音效从 48000 改成 44100，一是避开重采样开销，
 * 二就是让这里只有一个采样率。
 */
#define AUDIO_STREAM_SAMPLE_RATE    44100u

/* 一次 DMA 回调对应的毫秒数（仅用于日志，不参与累计） */
#define AUDIO_DMA_MS        ((AUDIO_DMA_SAMPLES * 1000u) / AUDIO_STREAM_SAMPLE_RATE)

/*
 * MP3 播放音量（0..15，经 g_music_vol_level[] 映射成 dB，见 drv_audprc.c:55）。
 * 2026-10-31 实测选定：上电依次用 8 / 6 / 4 三档各播 2 秒，学生听感判定
 * **4 档（约 -28dB）最合适**，故定为 4。
 *
 * ⚠ 若将来启用软件混音（CONFIG_SOFTWARE_TX_MIX_ENABLE），BGM 与音效会
 *   共用同一个音量槽 private_volume[AUDIO_TYPE_LOCAL_MUSIC]，而 sfx_init()
 *   在 audio_start_async() 之后执行并再设一次，那时实际生效的是 sfx.c 的
 *   SFX_VOLUME。两处数值必须一起改。
 */
#define AUDIO_MUSIC_VOLUME  4u

/* 上电自测播放开关：1 = 开（调试用，上电循环播 5 轮×8 秒），0 = 关
 *
 * 2026-10-31：REQ-008 阶段 2 接入场景后必须置 0 —— BGM 已改为在点击进入 PLAYING 时
 * 由 audio_play_async() 启动，自测再抢一次会让两者互相 close/open。
 */
#define AUDIO_BOOT_TEST     0

/* 由 src/mp3_sample.c 提供（自动生成，见该文件头注释） */
extern const unsigned char g_mp3_sample[];
extern const unsigned int  g_mp3_sample_len;

static mp3ctrl_handle s_mp3;
static int s_inited;
static volatile uint32_t s_dma_ticks;   /* DMA 回调累计次数 */
static volatile int s_playing;
static volatile int s_finished;

/*
 * 音频 TX DMA 回调。名字叫 10ms，但实际周期是「一次 DMA 的样本数 ÷ 采样率」，
 * 开混音时为 160 样本 / 44100Hz ≈ 3.63ms（详见上方 AUDIO_DMA_SAMPLES 注释）。
 * 因此这里只累加次数，换算成毫秒统一走 audio_pos_ms()。
 */
static void audio_dma_cb(void *user_data)
{
    (void)user_data;
    if (s_playing)
        s_dma_ticks++;
}

/* mp3ctrl 的解码回调：只用来感知「播完了」 */
static int audio_mp3_cb(audio_server_callback_cmt_t cmd, void *user_data, uint32_t reserved)
{
    (void)user_data;
    (void)reserved;

    if (cmd == as_callback_cmd_play_to_end)
    {
        s_playing = 0;
        s_finished = 1;
        rt_kprintf("[AUDIO] play to end at %ums\n", (unsigned)audio_pos_ms());
    }
    return 0;
}

int audio_init(void)
{
    if (s_inited)
        return 0;

    /*
     * 只注册 DMA 回调（用于测已播时长），不做任何阻塞操作。
     * 曲目数据已在固件里（g_mp3_sample），不依赖 TF 卡，
     * 所以这里不会出现 CR-05 之前的启动期卡死问题。
     */
    audio_register_10ms_tx_dma_callback(audio_dma_cb, RT_NULL);

    /* MP3 走 LOCAL_MUSIC 的私有音量通道，与 SDK 例程一致 */
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_MUSIC_VOLUME);

    s_inited = 1;
    rt_kprintf("[AUDIO] ready: in-memory mp3, %u bytes, volume=%u, 10ms DMA callback registered\n",
               g_mp3_sample_len, (unsigned)AUDIO_MUSIC_VOLUME);
    return 0;
}

int audio_play(void)
{
    if (!s_inited)
        return -1;

    if (s_mp3)
    {
        mp3ctrl_close(s_mp3);
        s_mp3 = RT_NULL;
    }

    s_dma_ticks = 0;
    s_finished = 0;
    s_playing = 1;

    /* 直接从 flash 里的数组解码，不需要挂文件系统 */
    s_mp3 = mp3ctrl_open_buffer(AUDIO_TYPE_LOCAL_MUSIC,
                                (const char *)g_mp3_sample,
                                g_mp3_sample_len,
                                audio_mp3_cb, RT_NULL);
    if (!s_mp3)
    {
        s_playing = 0;
        rt_kprintf("[AUDIO] ERROR: mp3ctrl_open_buffer failed\n");
        return -1;
    }

    /* mp3ctrl_open_buffer 内部会 audio_open2 一个 client，很吃堆，量一下 */

    /*
     * 必须显式调用 mp3ctrl_play() 才真正开始播放。
     * mp3ctrl_open_buffer() 只负责创建句柄并启动解码线程（audio_mp3ctrl.c:1903
     * rt_thread_startup），不会自动出声 —— 2026-10-31 实测：只 open 不 play 时
     * audio_dma_cb 一次都不触发，pos 恒为 0，完全无声。
     * 注意 mp3ctrl_play() 内部会 rt_event_recv(RT_WAITING_FOREVER) 等播放确认，
     * 属于阻塞调用，只应在后台线程里用。
     */
    if (mp3ctrl_play(s_mp3) != 0)
    {
        s_playing = 0;
        rt_kprintf("[AUDIO] ERROR: mp3ctrl_play failed\n");
        mp3ctrl_close(s_mp3);
        s_mp3 = RT_NULL;
        return -1;
    }

    rt_kprintf("[AUDIO] playing in-memory mp3 (%u bytes)\n", g_mp3_sample_len);
    return 0;
}

/*
 * 异步启动播放（REQ-008 场景接入用）。
 *
 * **必须异步**：mp3ctrl_play() 内部是 rt_event_recv(RT_WAITING_FOREVER)，
 * 一直阻塞到音频真的开播才返回（见 audio_mp3ctrl.c:2012-2025）。
 * 如果直接在帧循环里调 audio_play()，整个帧会被卡住，连画面都不刷。
 * 所以这里起一个短命线程去播，线程入口返回后由 RT-Thread 自动回收。
 */
static void audio_play_thread(void *param)
{
    (void)param;
    audio_play();
}

int audio_play_async(void)
{
    rt_thread_t tid;

    if (!s_inited)
        return -1;

    tid = rt_thread_create("aplay", audio_play_thread, RT_NULL, 2048, 22, 10);
    if (!tid)
    {
        rt_kprintf("[AUDIO] ERROR: create play thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    return 0;
}

uint32_t audio_pos_ms(void)
{
    /*
     * 已播时长 = 回调次数 × 每次样本数 ÷ 采样率。
     * 用 64 位中间量避免长曲子（例如 3 分钟 = 34000+ 次回调）溢出。
     */
    uint64_t samples = (uint64_t)s_dma_ticks * AUDIO_DMA_SAMPLES;
    return (uint32_t)((samples * 1000u) / AUDIO_STREAM_SAMPLE_RATE);
}

void audio_stop(void)
{
    s_playing = 0;
    if (s_mp3)
    {
        mp3ctrl_close(s_mp3);
        s_mp3 = RT_NULL;
        rt_kprintf("[AUDIO] stopped at %ums\n", (unsigned)audio_pos_ms());
    }
}

int audio_is_playing(void)
{
    return s_playing;
}

int audio_finished(void)
{
    return s_finished;
}

int audio_is_mounted(void)
{
    return s_inited;
}

/*
 * 阶段 1 自测：用固定音量播放固件内置的测试曲目一次（约 8 秒）。
 *
 * 目的：
 *   1) 证明 MP3 能从内存解码出声（已验证）；
 *   2) 更重要的 —— 让 PCM 流持续存在，好验证 REQ-009 的命中音效能否
 *      通过 audio_set_pcm_callback() 钩子叠加到音乐上（混音）。
 *      音效依赖 PCM 流，**没有音乐时音效不会响**。
 * 正式接入谱面同步后关闭（AUDIO_BOOT_TEST 置 0）。
 */
void audio_boot_test(void)
{
    int i;

    if (!s_inited)
        return;

    if (!AUDIO_BOOT_TEST)
        return;

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_MUSIC_VOLUME);

    rt_kprintf("[AUDIO] boot test: loop play at volume %u/15 (about 40s window)\n",
               (unsigned)AUDIO_MUSIC_VOLUME);

    /*
     * 循环播 5 轮（每轮约 8 秒），给学生足够时间进游戏敲音符验证混音。
     * 测完直接断电即可；正式版接谱面同步后这段会关掉。
     */
    for (int round = 0; round < 5; round++)
    {
        rt_kprintf("[AUDIO] round %d start\n", round + 1);

        if (audio_play() != 0)
        {
            rt_kprintf("[AUDIO] boot test: play failed\n");
            return;
        }

        for (i = 0; i < 16; i++)
        {
            rt_thread_mdelay(500);
            rt_kprintf("[AUDIO] pos=%ums playing=%d\n",
                       (unsigned)audio_pos_ms(), audio_is_playing());
            if (!audio_is_playing())
                break;
        }

        audio_stop();
        rt_thread_mdelay(300);
    }

    rt_kprintf("[AUDIO] boot test done at %ums\n", (unsigned)audio_pos_ms());
}

/*
 * 异步启动入口（保留此接口以免调用方改动）。
 *
 * 已经不涉及 TF 卡握手，初始化本身不阻塞，这里直接同步调用即可；
 * 自测在独立线程里跑，避免阻塞启动流程。
 */
static void audio_init_thread(void *param)
{
    (void)param;

    rt_thread_mdelay(3000);
    audio_boot_test();
}

int audio_start_async(void)
{
    rt_thread_t tid;

    /* 初始化本身不阻塞（只注册 DMA 回调），先同步做完 */
    if (audio_init() != 0)
        return -1;

    /* 自测放到独立线程，避免 5 秒的播放验证拖慢启动 */
    tid = rt_thread_create("audio_test", audio_init_thread, RT_NULL, 4096, 22, 10);
    if (!tid)
    {
        rt_kprintf("[AUDIO] WARN: create audio_test thread failed, skip boot test\n");
        return 0;
    }
    rt_thread_startup(tid);
    rt_kprintf("[AUDIO] init ok, boot test thread started\n");
    return 0;
}
