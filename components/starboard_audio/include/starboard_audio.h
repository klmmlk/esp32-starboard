#ifndef STARBOARD_AUDIO_H
#define STARBOARD_AUDIO_H

#include <stdint.h>

// =============================================================================
// starboard_audio —— MAX98357A I2S 功放驱动(提示音合成 + 本地 MP3 播放)
//
// 硬件: MAX98357A(单声道 I2S 功放,免 MCLK,内置 DAC + D 类放大),三线 BCLK/LRC/DIN。
//        GAIN/SD 脚未接 GPIO(靠硬件默认增益,放完音停 I2S 即可,无需软件控功放)。
// 驱动: ESP-IDF 原生 driver/i2s_std(Philips 标准格式,16bit)。MP3 用 libhelix-mp3 解码。
// 暴露: 全局对象 audio(对齐 hal/display)。播放跑在独立任务(主任务栈仅 3584 不够)。
// 回合制契合:
//   - tone():同步阻塞(短提示音,几百 ms)——播完才返回。
//   - playMp3():异步启动,App setup 启动后进按键轮询(isPlaying/stop),任意键停或播完;
//     播放期间 setup 不返回 → 不进保持期 → 不深睡。
// =============================================================================

class Audio
{
public:
    /** 配置 I2S TX 通道 + 启动播放任务。
     *  ⚠️ 必须在 display_init() 之后调用:GPIO13(BCLK)原是屏幕 SPI 默认 MISO,
     *     I2S init 会重新接管该引脚(屏幕不读 MISO,无影响)。 */
    void init();
    /** 释放 I2S 通道(深睡靠硬件掉电自动停,一般无需显式调)。*/
    void deinit();

    /** 合成正弦提示音。freq 频率 Hz,ms 持续。同步阻塞到播完(音量取当前 volume)。*/
    void tone(uint32_t freq, uint32_t ms);
    /** 异步启动播放 LittleFS 上的 MP3。path 非 '/' 开头自动补 /littlefs/ 前缀。
     *  返回是否成功启动(文件可打开即视为成功)。*/
    bool playMp3(const char *path);
    /** 是否正在播放 MP3。*/
    bool isPlaying() const { return playing; }
    /** 停止当前播放(置停止标志,等任务退出播放循环)。*/
    void stop();
    /** 音量 0..21(软件缩放 PCM;GAIN 未接故用软件)。*/
    void setVolume(uint8_t v) { volume = (v > 21) ? 21 : v; }
    uint8_t getVolume() const { return volume; }

private:
    enum class Cmd : uint8_t { None, Tone, Mp3 };
    void i2sSetup(uint32_t sampleRate);  // 配/重配 I2S(STEREO 16bit,给定采样率)
    void doTone();                       // 任务内:合成正弦播放
    void doPlayMp3();                    // 任务内:libhelix 解码播放
    static void taskEntry(void *arg);    // FreeRTOS 任务入口
    void runLoop();                      // 任务主循环

    volatile Cmd cmd = Cmd::None;
    volatile bool stopFlag = false;
    volatile bool playing = false;
    void *taskHandle = nullptr;   // TaskHandle_t(避免头里 include freertos)
    void *callerHandle = nullptr; // tone 等待完成时记录调用者任务
    uint32_t toneFreq = 1000;
    uint32_t toneMs = 200;
    char mp3Path[96] = {0};
    uint8_t volume = 12;
    int16_t sineTable[256] = {0};
    bool i2sInited = false;       // I2S 通道是否已 init_std_mode(只能调一次)
    uint32_t curRate = 0;         // 当前采样率(换率用 reconfig_std_clock,不重复 init)
    bool channelEnabled = false;  // I2S 通道是否已 enable(disable 前检查,避免 "not enabled yet" 报错)
};

extern Audio audio;

#endif // STARBOARD_AUDIO_H
