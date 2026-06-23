// starboard_hal 实现 —— 见 include/starboard_hal.h
// 参考移植自 LiClock/src/hal.cpp,已剥离 display/peripherals 依赖。
//
// 当前进度: M1(init + 按键)。其余成员(getTime/电压/WiFi/深睡)后续里程碑补。

#include "starboard_hal.h"

// -----------------------------------------------------------------------------
// 按键事件回调(M1 验证用:串口打印)。后续里程碑会被 App 框架接管。
// -----------------------------------------------------------------------------
static void onBtnLClick() { Serial.println("[HAL] 左键 短按"); }
static void onBtnCClick() { Serial.println("[HAL] 中键 短按"); }
static void onBtnRClick() { Serial.println("[HAL] 右键 短按"); }
static void onBtnLLongPress() { Serial.println("[HAL] 左键 长按"); }
static void onBtnCLongPress() { Serial.println("[HAL] 中键 长按"); }
static void onBtnRLongPress() { Serial.println("[HAL] 右键 长按"); }

// 周期 tick 三个按键的任务。OneButton 需要被频繁 tick 才能检测点击/长按。
static void task_hal_update(void *)
{
    while (true)
    {
        hal.tickButtons();
        vTaskDelay(pdMS_TO_TICKS(20)); // 20ms 轮询,足够检测人手速度
    }
}

void HAL::tickButtons()
{
    btnl.tick();
    btnc.tick();
    btnr.tick();
}

void HAL::waitForAllReleased()
{
    while (btnl.isPressed() || btnc.isPressed() || btnr.isPressed())
    {
        tickButtons();
        delay(10);
    }
}

void HAL::init()
{
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("================ starboard_hal init ================");

    // 时区: 东八区
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // NVS 配置存储
    pref.begin("starboard", false);
    Serial.println("[HAL] Preferences 已打开(namespace=starboard)");

    // 按键事件(M1 验证):后续由 App 框架接管,这里先串口打印
    btnl.attachClick(onBtnLClick);
    btnc.attachClick(onBtnCClick);
    btnr.attachClick(onBtnRClick);
    btnl.attachLongPressStart(onBtnLLongPress);
    btnc.attachLongPressStart(onBtnCLongPress);
    btnr.attachLongPressStart(onBtnRLongPress);

    // 启动按键轮询任务
    xTaskCreate(task_hal_update, "hal_update", 4096, nullptr, 5, nullptr);
    Serial.println("[HAL] 按键轮询任务已启动");
    Serial.println("[HAL] init 完成(M1)。按动左/中/右键,串口将打印事件。");
}

void HAL::update()
{
    // M2 起补: 电压采样/充电状态。
}

HAL hal;
