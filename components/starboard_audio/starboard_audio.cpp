// starboard_audio 实现 —— 见 include/starboard_audio.h
//
// I2S: ESP-IDF driver/i2s_std 新版 API(channel alloc → init_std_mode → enable → write)。
// MP3: libhelix-mp3(MP3InitDecoder/MP3FindSyncWord/MP3Decode/MP3GetLastFrameInfo)解码
//      → int16 PCM → 按 samprate 重配 I2S → 转立体声(单声道左右复制)+ 音量缩放 → write。
// 解码/合成跑在独立任务(8192 栈,主任务栈仅 3584 不够)。

#include "starboard_audio.h"
#include "starboard_config.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_err.h"
extern "C" {
#include "mp3dec.h"
}

Audio audio; // 全局实例(对齐 hal/display)

// I2S TX 句柄(文件内静态,init/deinit/i2sSetup 共用)
static i2s_chan_handle_t s_tx = nullptr;

// ----------------------------- 公开 API -----------------------------

void Audio::init()
{
    // 正弦查表(提示音合成用,定点相位累加器的高 8 位索引)
    for (int i = 0; i < 256; i++)
        sineTable[i] = (int16_t)(sinf(2.0f * 3.14159265f * i / 256.0f) * 32767.0f);

    // I2S 通道(只 new 一次)
    if (!s_tx)
    {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK)
        {
            Serial.println("[AUDIO] i2s_new_channel failed");
            return;
        }
    }
    i2sSetup(48000); // 默认 48kHz(tone 用;playMp3 首帧后按 MP3 采样率重配)

    // 播放任务(8192 栈,优先级 2 低于按键)
    if (!taskHandle)
        xTaskCreate(taskEntry, "audio", 8192, this, 2, (TaskHandle_t *)&taskHandle);
}

void Audio::deinit()
{
    stop();
    if (s_tx)
    {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
}

void Audio::tone(uint32_t freq, uint32_t ms)
{
    if (!taskHandle || !s_tx) return;
    callerHandle = xTaskGetCurrentTaskHandle();
    toneFreq = freq; toneMs = ms; stopFlag = false; cmd = Cmd::Tone;
    xTaskNotifyGive((TaskHandle_t)taskHandle);
    // 阻塞等 doTone 完成(调用者任务上下文,符合回合制:播完才返回)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

bool Audio::playMp3(const char *path)
{
    if (!taskHandle || !s_tx) return false;
    if (playing) stop(); // 切歌:先停当前
    if (path[0] == '/') strncpy(mp3Path, path, sizeof(mp3Path) - 1);
    else snprintf(mp3Path, sizeof(mp3Path), "/littlefs/%s", path);
    mp3Path[sizeof(mp3Path) - 1] = 0;
    // 先验文件可打开(避免任务空跑)
    FILE *test = fopen(mp3Path, "rb");
    if (!test) return false;
    fclose(test);
    stopFlag = false; playing = true; cmd = Cmd::Mp3;
    xTaskNotifyGive((TaskHandle_t)taskHandle);
    return true;
}

void Audio::stop()
{
    stopFlag = true;
    for (int i = 0; i < 200 && playing; i++) delay(10); // 等任务退出播放循环(≤2s)
}

// ----------------------------- 内部 -----------------------------

void Audio::i2sSetup(uint32_t sampleRate)
{
    if (!s_tx) return;
    if (channelEnabled) { i2s_channel_disable(s_tx); channelEnabled = false; } // reconfig/init 前须 disable

    // init_std_mode 只能调一次:首次完整初始化(含 slot/gpio),之后只换采样率
    if (!i2sInited)
    {
        i2s_std_config_t std_cfg;
        std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000);
        std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;            // MAX98357A 免 MCLK
        std_cfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
        std_cfg.gpio_cfg.ws   = (gpio_num_t)PIN_LRCLK;
        std_cfg.gpio_cfg.dout = (gpio_num_t)PIN_DIN;
        std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
        std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
        std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
        std_cfg.gpio_cfg.invert_flags.ws_inv   = false;
        esp_err_t e = i2s_channel_init_std_mode(s_tx, &std_cfg);
        if (e != ESP_OK) Serial.printf("[AUDIO] init_std_mode=%d\n", e);
        i2sInited = true;
        curRate = 48000;
    }
    // 采样率变化 → reconfig clock(不需重新 init 整个通道,避免 "initialized already" 报错)
    if (sampleRate != curRate)
    {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
        esp_err_t e = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
        if (e != ESP_OK) Serial.printf("[AUDIO] reconfig_clock(%lu)=%d\n", (unsigned long)sampleRate, e);
        curRate = sampleRate;
    }
    i2s_channel_enable(s_tx);
    channelEnabled = true;
}

void Audio::taskEntry(void *arg)
{
    static_cast<Audio *>(arg)->runLoop();
}

void Audio::runLoop()
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等 tone/playMp3 唤醒
        Cmd c = cmd;
        if (c == Cmd::Tone)
        {
            doTone();
            cmd = Cmd::None;
            if (callerHandle) xTaskNotifyGive((TaskHandle_t)callerHandle); // 唤醒 tone 调用者
        }
        else if (c == Cmd::Mp3)
        {
            doPlayMp3();
            cmd = Cmd::None;
            playing = false;
        }
    }
}

// 合成正弦提示音:定点相位累加器 + 查表,立体声(左右相同,MAX98357A 取左)。
void Audio::doTone()
{
    const uint32_t sampleRate = 48000;
    i2sSetup(sampleRate);
    uint32_t phaseStep = ((uint64_t)toneFreq << 24) / sampleRate; // Q8.24 定点步进
    uint32_t phaseAcc = 0;
    const int BLOCK = 1024; // 512 立体声对 = 2048 字节
    int16_t buf[BLOCK];
    int pairsTotal = (int)((uint64_t)sampleRate * toneMs / 1000);
    int pairsDone = 0;
    while (!stopFlag && pairsDone < pairsTotal)
    {
        for (int i = 0; i < BLOCK; i += 2)
        {
            int16_t s = (int16_t)((int32_t)sineTable[(phaseAcc >> 24) & 0xFF] * (int32_t)volume / 21);
            buf[i] = s; buf[i + 1] = s;
            phaseAcc += phaseStep;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, BLOCK * 2, &written, portMAX_DELAY);
        pairsDone += BLOCK / 2;
    }
    if (channelEnabled) { i2s_channel_disable(s_tx); channelEnabled = false; } // 停 DMA,避免循环重复末帧导致持续蜂鸣
}

// libhelix 解码 MP3:分块读文件 → 找同步字 → 解码一帧 → 按帧 samprate/nChans 配 I2S
// → 转 16bit 立体声 PCM + 音量缩放 → I2S write。抄 testwrap/main.c 范式。
void Audio::doPlayMp3()
{
    FILE *f = fopen(mp3Path, "rb");
    if (!f) return;
    HMP3Decoder dec = MP3InitDecoder();
    const int IN_SIZE = 2 * MAINBUF_SIZE; // 3880
    uint8_t *inBuf  = (uint8_t *)malloc(IN_SIZE);
    short   *outBuf = (short *)malloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * sizeof(short));     // 2304 short
    short   *stereo = (short *)malloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP * 2 * sizeof(short)); // 立体声缓冲
    if (!dec || !inBuf || !outBuf || !stereo)
    {
        if (dec) MP3FreeDecoder(dec);
        free(inBuf); free(outBuf); free(stereo);
        fclose(f);
        Serial.println("[AUDIO] playMp3: alloc/decoder failed");
        return;
    }

    int bytesLeft = 0;
    uint8_t *readPtr = inBuf;
    bool i2sReady = false;
    uint32_t curRate = 0;
    uint8_t curChan = 0;

    while (!stopFlag)
    {
        // 缓冲不足 → 把残余移到头续读(解码会前移 readPtr/扣 bytesLeft)
        if (bytesLeft < MAINBUF_SIZE)
        {
            if (readPtr > inBuf) { memmove(inBuf, readPtr, bytesLeft); readPtr = inBuf; }
            int n = fread(inBuf + bytesLeft, 1, IN_SIZE - bytesLeft, f);
            if (n > 0) bytesLeft += n;
            else if (bytesLeft <= 0) break; // EOF 且无残余
        }
        if (bytesLeft <= 0) break;

        int offset = MP3FindSyncWord(readPtr, bytesLeft);
        if (offset < 0) break;
        readPtr += offset; bytesLeft -= offset;

        int err = MP3Decode(dec, &readPtr, &bytesLeft, outBuf, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) continue;                                       // 数据不足,下轮回填
        if (err) { if (bytesLeft > 0) { readPtr++; bytesLeft--; } continue; }                 // 坏帧,强跳 1 字节防死循环

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        if (info.outputSamps <= 0) continue;

        // 首帧 / 格式变 → 重配 I2S 采样率(避免音调失真)
        if (!i2sReady || info.samprate != curRate || info.nChans != curChan)
        {
            curRate = info.samprate; curChan = info.nChans;
            i2sSetup(curRate);
            i2sReady = true;
            Serial.printf("[AUDIO] mp3 %luHz %dch\n", (unsigned long)curRate, curChan);
        }

        // 转 16bit 立体声(单声道左右复制,MAX98357A 取左)+ 音量缩放
        int stereoSamps;
        if (info.nChans == 1)
        {
            for (int i = 0; i < info.outputSamps; i++)
            {
                short s = (short)((int32_t)outBuf[i] * volume / 21);
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            stereoSamps = info.outputSamps * 2;
        }
        else
        {
            for (int i = 0; i < info.outputSamps; i++)
                stereo[i] = (short)((int32_t)outBuf[i] * volume / 21);
            stereoSamps = info.outputSamps;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, stereo, stereoSamps * 2, &written, portMAX_DELAY);
    }

    if (channelEnabled) { i2s_channel_disable(s_tx); channelEnabled = false; } // 停 DMA,避免循环重复末帧
    MP3FreeDecoder(dec);
    free(inBuf); free(outBuf); free(stereo);
    fclose(f);
}
