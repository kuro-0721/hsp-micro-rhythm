/*
 * sfx.c —— 命中音效（REQ-009 / ADR-0005）
 *
 * REQ-009 原定「命中触发马达震动」，因马达需外购焊接而改为「命中触发特殊音效」，
 * 并要求客观判据（命中时刻与音效触发时间戳偏差 ≤20ms），而不是「能不能听见」。
 *
 * 【为什么音效是代码合成的】
 *   如果用音频文件，就要挂文件系统（DFS/ELMFAT）、预置资源到 flash 分区，
 *   而 P0 单曲本身是无声的、TF 卡要到 P1 才有。合成音效不依赖任何外部资源，
 *   既躲开这套复杂度，也天然满足 REQ-105「素材自制」。
 *
 * 【波形设计】
 *   - 440Hz 方波（学生选的「低一点」，比 880Hz 低一个八度）
 *   - 一阶低通（y += (x-y)/4）让方波不那么刺耳
 *   - 指数衰减包络（每样本 env -= env>>7，半衰约 5.5ms），50ms 内衰减到接近 0
 *   - 全整数运算，不用 math.h、不依赖 libm
 *
 * 【播放模型：回调驱动（2026-10-25 修正）】
 *   最初写成「audio_open + 独立线程 audio_write 一次」，烧到板上完全没声音。
 *   查 SDK 例程（dual_adc_dac / opus / flac / mixer / local_music）后确认正确用法是：
 *   audio_server 在写缓存半空/空时通过 callback 要数据
 *   （as_callback_cmd_cache_half_empty / as_callback_cmd_cache_empty），
 *   应用在 callback 里 audio_write 补齐。空 callback + 偶尔主动写 1600 字节，
 *   缓存旋即排空，DAC 没有数据可播，于是无声。
 *   现在改为：callback 里持续补数据 —— 有音效待播就填音效样本，否则填静音。
 *
 * 【为什么没有音效时也要一直喂静音】
 *   音频链路一旦断流就不会自己恢复，只有持续有数据 DAC 才保持运行，
 *   触发瞬间才能立刻出声。代价是音频链路常开（功耗与底噪），作为已知限制记入 docs/06。
 *
 * 【延迟与 cache 大小】
 *   发声延迟 ≈ 等待下一次 cache 半空回调的时间。
 *   cache 1024 字节（16kHz / 16bit 单声道 = 32ms 数据）→ 最坏等 16ms，
 *   才可能满足 REQ-009 的 ≤20ms。若沿用例程常见的 2048 字节，最坏要等 32ms，必然超标。
 *
 * 【判据的局限，必须写清楚】
 *   sfx_trigger() 返回的时间戳是「决定要发声的那一刻」，不是「声波真正发出」的时刻。
 *   本版另外记录了 sfx_feed_latency_ms()：音效第一个样本进入音频缓存的时刻与
 *   trigger 时刻的差 —— 比 trigger 本身更接近真实发声，但仍不含 DAC 与喇叭的时延。
 *   要测到「声波真正发出」需要用 audio_register_10ms_tx_dma_callback，本版未做。
 */
#include "app.h"
#include "audio_server.h"

/*
 * 采样率用 48000 而非 16000，原因（2026-10-25 实测定位）：
 *   drv_audprc 在 16000/8000 时打印 "samplerate = 16000, select voice eq"，
 *   即启用**语音专用 EQ**。voice EQ 内含 AGC（自动增益控制）：持续有信号时
 *   它会一步步把增益往上顶，静音后回落。用常驻 audio client 时表现为
 *   「敲一次响一点，静音后再敲又变小」——这正是 AGC 的典型行为。
 *   改用 48000 走 music EQ，绕开语音 AGC。
 *   代价：PCM 样本数变 3 倍（50ms × 48kHz = 2400 样本，约 4.8KB，RAM 可接受）。
 *
 * 2026-10-31 改：48000 → 44100。
 *   开启软件混音后，audio_server 的混音目标采样率由**第一个打开的 client** 决定
 *   （audio_server.c:2497），其余 client 都要 sifli_resample 到它。
 *   sfx 是第一个打开的，原本 48000 会强制 MP3(44100) 每 10ms 重采样一次，
 *   白白多吃一份 CPU（实测帧间隔 p95 从 34ms 恶化到 36ms）。
 *   改成 44100 后两路同采样率，走 audio_write_resample 的直通分支，不重采样。
 *   44100 同样不是 16000/8000，依旧走 music EQ，不会重新引入语音 AGC。
 */
#define SFX_SAMPLE_RATE     44100u
#define SFX_TONE_HZ         440u
#define SFX_DURATION_MS     50u
/*
 * 2026-10-31 改：11000 → 30000（配合软件混音）
 *   开启 CONFIG_SOFTWARE_TX_MIX_ENABLE 后，服务端把两路 PCM 做 (a>>1)+(b>>1)，
 *   每一路都被砍 6dB；同时 BGM 和音效共用同一个音量槽（见下方 SFX_VOLUME），
 *   该槽按音乐能接受的档位取（-28dB）。BGM 是正常录音电平，本音效是代码合成的
 *   小幅度信号，若不同时抬高幅度，命中声会被 BGM 完全盖住。
 *   30000/32767 ≈ 92%，混音后峰值 ≈ 15000 + BGM 半幅 16384 = 31384 < 32767，不削顶。
 *   这是首版取值，听感不合适就调本宏，不要去改音量槽。
 */
#define SFX_AMPLITUDE       30000
/*
 * 0..15 经音量表映射（drv_audprc.c:51/55）：
 *   g_tel_vol_level  = {-36,-34,-32,-30,-28,-26,-24,-22,-20,-17,-14,-11,-10,-8,-6,-4}
 *   g_music_vol_level= {-55,-34,-32,-30,-28,-26,-24,-22,-20,-17,-14,-11,-10,-8,-6,-4}
 * 即 4→-28dB、8→-20dB、14→-6dB、15→-4dB。
 *
 * 注意：**不要用 15（即 AUDIO_MAX_VOLUME）**。
 * audio_server.c:380 有一段只在 vol == AUDIO_MAX_VOLUME 时才执行的特殊处理：
 *   decrease_level = eq_get_decrease_level(...); 然后按位移相加给 PCM 加额外增益。
 * 把音量拉到 15 就会踩进这条分支，实测表现为「每敲一次声音更大一次」；
 * 改成 14 即绕开该分支。
 *
 * 2026-10-31 改：14 → 4，因为开启软件混音后 BGM 和音效共用同一个音量槽。
 *   音量是「按 audio_type 存一份」（private_volume[AUDIO_TYPE_LOCAL_MUSIC]），
 *   不是按 client 存；两个客户端都是 LOCAL_MUSIC，且 speaker_update_volume()
 *   取 TX 队列里第一个 client 的 audio_type 定整个混音流的音量。
 *   最后调 audio_server_set_private_volume 的生效 —— sfx_init() 在
 *   audio_start_async() 之后跑，所以**真正生效的是本宏，不是 audio.c 的
 *   AUDIO_MUSIC_VOLUME**。
 *   取 4（-28dB）是学生 2026-10-31 用 8/6/4 三档试听后选定的 BGM 音量，
 *   两个宏必须一起改。
 */
#define SFX_VOLUME          4u
#define SFX_PCM_MAX         ((SFX_SAMPLE_RATE * SFX_DURATION_MS) / 1000u)
/*
 * 与老师验证过的 peripheral_lab/src/lab_audio.c 对齐：
 *   cache 4096 字节，callback 传 NULL，独立线程主动 audio_write。
 * 块大小按「实时速率」定：160 样本 @16kHz = 10ms 音频，与 10ms 周期 1:1，
 * 既不堆积也不 underrun。写成 256 样本（25600 样本/秒）会超过消费速率
 * 16000 样本/秒，缓存写满后 audio_write 返回 0，样本被丢弃。
 */
#define SFX_CACHE_BYTES     4096u
#define SFX_BLOCK_SAMPLES   441u        /* 10ms @ 44.1kHz，与 10ms 周期 1:1 */
#define SFX_BLOCK_MS        10u
#define SFX_THREAD_PRIO     24          /* 必须低于帧循环 game(20)，见 R-09 */
/* 上电自检音模式：0 = 关闭（正式版）；1 = 常驻 client + 20 声连击自检；2 = 完整周期对照实验 */
#define SFX_BOOT_SELFTEST   0

static int16_t s_pcm[SFX_PCM_MAX];
static int16_t s_block[SFX_BLOCK_SAMPLES];
static uint32_t s_pcm_samples;
static audio_client_t s_client;
static int s_enabled = 1;
static uint32_t s_last_trigger_ms;

/* 以下由音频线程与判定线程共享，均加 volatile */
static volatile uint32_t s_play_pos;        /* 音效已喂到第几个样本 */
static volatile int s_playing;              /* 1 = 还有音效样本没喂完 */
static volatile uint32_t s_write_ok;        /* audio_write 返回 >0（写入成功）的次数 */
static volatile uint32_t s_write_zero;      /* 返回 0（缓存满）的次数 */
static volatile int s_last_write_ret;       /* 最近一次 audio_write 返回值 */
static volatile uint32_t s_feed_latency_ms; /* 首样本写入 - trigger 的毫秒差 */
static volatile uint32_t s_trigger_count;   /* 触发总次数，用于周期打印音量诊断 */
static volatile uint32_t s_skip_count;      /* 因上一段未播完而跳过的触发次数 */

/* 启动时合成一次，之后只读。50ms × 16kHz = 800 个样本。 */
static void synthesize(void)
{
    uint32_t n = SFX_PCM_MAX;
    /* 相位步进：(f / fs) 用 32 位定点表示，累加溢出即完成一个周期 */
    uint32_t step = (uint32_t)(((uint64_t)SFX_TONE_HZ << 32) / SFX_SAMPLE_RATE);
    uint32_t phase = 0;
    int32_t env = 32768;
    int32_t lp = 0;

    for (uint32_t i = 0; i < n; i++)
    {
        int32_t sq;
        int32_t v;

        phase += step;
        sq = (phase & 0x80000000u) ? SFX_AMPLITUDE : -SFX_AMPLITUDE;

        /* 一阶低通：把方波的硬边磨圆，听感不那么刺耳 */
        lp += (sq - lp) / 4;
        v = lp;

        /* 指数衰减包络 */
        v = (int32_t)(((int64_t)v * env) >> 15);
        env -= env >> 7;

        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        s_pcm[i] = (int16_t)v;
    }

    s_pcm_samples = n;
}

/*
 * 音频喂数据线程：每 10ms 写 160 样本（10ms 音频，实时速率 1:1）。
 * 有音效待播就填音效样本，否则填静音 —— 音频链路必须持续有数据，
 * 否则 DAC 会 underrun，触发瞬间也发不出声。
 *
 * 【背压处理（关键）】
 *   audio_write 返回「实际写入字节数」：>0 成功、0 = 缓存满、<0 错误。
 *   缓存满时必须把 s_play_pos / s_playing 回退到本次填充前的值，否则
 *   音效样本会被白白“消耗”却不进缓存 —— 这正是 2026-10-25 完全无声的
 *   直接原因：原来每 10ms 无条件写 256 样本（25600 样本/秒 > 16000 消费），
 *   缓存长期满，音效样本全被丢掉。
 */
static void sfx_thread(void *param)
{
    (void)param;

    for (;;)
    {
        uint32_t start_pos = s_play_pos;
        int was_playing = s_playing;
        int ret;

        for (uint32_t i = 0; i < SFX_BLOCK_SAMPLES; i++)
        {
            int16_t v = 0;

            if (s_enabled && s_playing && s_play_pos < s_pcm_samples)
            {
                if (s_play_pos == 0)
                    s_feed_latency_ms = rt_tick_get_millisecond() - s_last_trigger_ms;

                v = s_pcm[s_play_pos++];

                if (s_play_pos >= s_pcm_samples)
                    s_playing = 0;
            }
            s_block[i] = v;
        }

        ret = audio_write(s_client, (uint8_t *)s_block, SFX_BLOCK_SAMPLES * sizeof(int16_t));
        s_last_write_ret = ret;

        if (ret > 0)
        {
            s_write_ok++;
            rt_thread_mdelay(SFX_BLOCK_MS);
        }
        else
        {
            /* 缓存满：回退播放位置，等消费后再写，绝不丢弃音效样本 */
            s_play_pos = start_pos;
            s_playing = was_playing;
            s_write_zero++;
            rt_thread_mdelay(2);
        }
    }
}

#if SFX_BOOT_SELFTEST == 2
/*
 * 对照实验：完全照抄老师验证过的 peripheral_lab/src/lab_audio.c 流程 ——
 * open → 连续写 2 秒 PCM → audio_ioctl(FLUSH_TIME_MS) → 等待 → close。
 * 与「常驻 client + 持续写」不同，这一版每次都是完整生命周期。
 * 若这一版能出声而常驻版不能，问题就锁定在常驻模式本身。
 */
static void sfx_boot_test(void)
{
    audio_parameter_t p = {0};
    audio_client_t c;
    int16_t block[256];
    unsigned total = SFX_SAMPLE_RATE * 2;   /* 2 秒 */
    unsigned offset = 0;
    uint32_t drain = 0;
    int wrote;

    p.write_samplerate = SFX_SAMPLE_RATE;
    p.write_channnel_num = 1;
    p.write_bits_per_sample = 16;
    p.write_cache_size = 4096;

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, SFX_VOLUME);

    c = audio_open2(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &p, RT_NULL, RT_NULL, AUDIO_DEVICE_SPEAKER);
    if (!c)
    {
        rt_kprintf("[SFX] boottest: open FAILED\n");
        return;
    }
    rt_kprintf("[SFX] boottest: open ok, %u samples\n", total);

    while (offset < total)
    {
        unsigned n = total - offset;
        unsigned i;

        if (n > 256)
            n = 256;

        for (i = 0; i < n; i++)
        {
            unsigned pos = offset + i;
            unsigned ramp = (pos < 320) ? pos : ((total - pos < 320) ? (total - pos) : 320);
            int32_t v = ((pos * SFX_TONE_HZ / SFX_SAMPLE_RATE) & 1u) ? 6000 : -6000;

            block[i] = (int16_t)(v * (int32_t)ramp / 320);
        }

        wrote = audio_write(c, (uint8_t *)block, n * 2);
        if (wrote > 0)
        {
            offset += (unsigned)wrote / 2;
            rt_thread_mdelay(1);
        }
        else if (wrote == 0)
        {
            rt_thread_mdelay(5);
        }
        else
        {
            rt_kprintf("[SFX] boottest: write error %d at %u\n", wrote, offset);
            break;
        }
    }

    if (audio_ioctl(c, AUDIO_IOCTL_FLUSH_TIME_MS, &drain) != 0)
        drain = 300;
    rt_thread_mdelay((drain > 1000) ? 1000 : drain + 200);
    audio_close(c);
    rt_kprintf("[SFX] boottest: done, %u samples, drain=%u ms\n", offset, (unsigned)drain);
}
#endif /* SFX_BOOT_SELFTEST == 2 */

int sfx_init(void)
{
    audio_parameter_t pa = {0};

    synthesize();

#if SFX_BOOT_SELFTEST == 2
    /* 对照实验模式：只跑完整周期，不启动常驻 client / 线程 */
    sfx_boot_test();
    return 0;
#endif

    pa.write_bits_per_sample = 16;
    pa.write_channnel_num = 1;
    pa.write_samplerate = SFX_SAMPLE_RATE;
    pa.write_cache_size = SFX_CACHE_BYTES;

    /*
     * 关闭 audprc 硬件 EQ（drv_audprc.c:2196 提供的公开接口）。
     *
     * 原因：这块芯片的 voice EQ / music EQ 内含 AGC（自动增益控制）。
     * 常驻 audio client 意味着音频链路一直在跑，AGC 会在持续有信号时把增益
     * 一级级抬上去，静音后又回落 —— 表现为「每敲一次响一点，静音后再敲又变小」。
     * 2026-10-25 实测确认：16kHz(voice eq) 与 48kHz(music eq) 都有此现象。
     *
     * config_eq_para() 第一件事就是 if (g_eq_enable == 0) return，
     * 所以这里设 0 后，后续 audio_open2 启动 stream 时不会再配置 EQ。
     * 必须在 audio_open2 之前调用才有效。
     */
    {
        extern void bf0_audprc_eq_enable_offline(uint8_t is_enable);

        bf0_audprc_eq_enable_offline(0);
    }

    /*
     * 必须用 audio_open2 并显式指定 AUDIO_DEVICE_SPEAKER，且 callback 传 NULL。
     *
     * 1) audio_open() 内部默认 device = AUDIO_DEVICE_AUTO（audio_server.c:3896），
     *    只有「录音请求」或「BT_VOICE / is_need_3a」才强制切 SPEAKER。
     *    本模块是 LOCAL_MUSIC + AUDIO_TX，三个条件都不满足，设备停在 AUTO。
     * 2) callback 传 NULL：老师验证过的 lab_audio.c 就是 NULL + 线程主动 write。
     *    先前用非 NULL callback，诊断数值全部正常却完全无声。
     * 类型必须是 AUDIO_TYPE_LOCAL_MUSIC（SDK 所有例程一致，无一处用 NOTIFY）。
     */
    s_client = audio_open2(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa, RT_NULL, RT_NULL,
                           AUDIO_DEVICE_SPEAKER);
    if (!s_client)
    {
        rt_kprintf("[SFX] ERROR: audio_open2 failed, hits will be silent\n");
        return -1;
    }

    /* 音量走私有的 LOCAL_MUSIC 通道，与 SDK 例程一致 */
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, SFX_VOLUME);

    {
        rt_thread_t tid = rt_thread_create("sfx", sfx_thread, RT_NULL, 2048, SFX_THREAD_PRIO, 10);

        if (!tid)
        {
            rt_kprintf("[SFX] ERROR: create thread failed\n");
            return -1;
        }
        rt_thread_startup(tid);
    }

    rt_kprintf("[SFX] ready: %uHz %ums %u samples, cache=%uB block=%u/%ums, volume=%u, enabled=%d\n",
               (unsigned)SFX_TONE_HZ, (unsigned)SFX_DURATION_MS,
               (unsigned)s_pcm_samples, (unsigned)SFX_CACHE_BYTES,
               (unsigned)SFX_BLOCK_SAMPLES, (unsigned)SFX_BLOCK_MS,
               (unsigned)SFX_VOLUME, s_enabled);

#if SFX_BOOT_SELFTEST
    /*
     * 连续触发 20 次（每次 50ms 音效、间隔 50ms）= 约 2 秒的连续「咚咚」声。
     * 单次 50ms 太短，人耳容易漏听，也无法区分「完全无声」与「很轻」。
     * 诊断解读：cb 不增长 = 回调没被调用；last_write_ret < 0 = 写失败；
     * 三者都正常却没声音 = 问题在缓存之后的 DA/功放/喇叭/供电。
     */
    rt_thread_mdelay(300);
    for (int i = 0; i < 20; i++)
    {
        sfx_trigger();
        rt_thread_mdelay(50);
    }
    rt_thread_mdelay(200);
    {
        uint32_t ok = 0, zero = 0;
        int lr = 0;

        sfx_get_diag(&ok, &zero, &lr);
        rt_kprintf("[SFX] selftest: write_ok=%u write_zero=%u last_write_ret=%d feed_lat=%ums\n",
                   (unsigned)ok, (unsigned)zero, lr,
                   (unsigned)sfx_feed_latency_ms());
    }
#endif
    return 0;
}

void sfx_set_enabled(int on)
{
    s_enabled = on ? 1 : 0;
    rt_kprintf("[SFX] %s\n", s_enabled ? "enabled" : "disabled");
}

int sfx_enabled(void)
{
    return s_enabled;
}

uint32_t sfx_trigger(void)
{
    uint32_t now = rt_tick_get_millisecond();

    s_last_trigger_ms = now;
    s_trigger_count++;

    if (s_enabled && s_client)
    {
        /*
         * 上一段音效还没写完就再次触发时，不重置播放位置 ——
         * 否则新的音效会从最大幅度重新开始，与缓存里剩下的旧样本叠在一起，
         * 听感就是「越敲越响」。密集连击（间隔 < 100ms）会明显暴露这个问题。
         */
        if (s_playing)
        {
            s_skip_count++;
        }
        else
        {
            s_play_pos = 0;
            s_playing = 1;
            s_feed_latency_ms = 0;
        }
    }

    /* 每 10 次触发打一行，拿来看音量有没有变（诊断「越敲越响」） */
    if ((s_trigger_count % 10u) == 1u)
        rt_kprintf("[SFX] vol=%u triggers=%u skipped=%u\n",
                   (unsigned)audio_server_get_private_volume(AUDIO_TYPE_LOCAL_MUSIC),
                   (unsigned)s_trigger_count, (unsigned)s_skip_count);

    return now;
}

uint32_t sfx_last_trigger_ms(void)
{
    return s_last_trigger_ms;
}

uint32_t sfx_feed_latency_ms(void)
{
    return s_feed_latency_ms;
}

void sfx_get_diag(uint32_t *write_ok, uint32_t *write_zero, int *last_write_ret)
{
    if (write_ok)
        *write_ok = s_write_ok;
    if (write_zero)
        *write_zero = s_write_zero;
    if (last_write_ret)
        *last_write_ret = s_last_write_ret;
}
