#ifndef STARBOARD_HAL_H
#define STARBOARD_HAL_H

#include <Arduino.h>
#include <Preferences.h>
#include "OneButton.h"
#include "starboard_config.h"

// =============================================================================
// starboard_hal —— 硬件抽象层
//
// 设计原则: 刻意【不】依赖 display/gui。HAL 是纯底层(按键/电源/WiFi/NTP/电压/蜂鸣器)。
//           需要显示交互的场景(配网选择界面/关机画面等)由上层 App 处理,通过 HAL 暴露的状态
//           或回调实现。这样 HAL 可独立编译/测试,也避免与 display 组件的循环依赖。
//
// 参考实现: LiClock/src/hal.cpp + include/hal.h(ESP32-Solo-1 版本)。
// 本组件按里程碑分批实现,见 docs/DEVELOPMENT.md 阶段1:
//   M1 init+按键  M2 配置+电压  M3 WiFi  M4 NTP  M5 深睡  M6 蜂鸣器
// =============================================================================

class HAL
{
public:
    // ------------------------- 生命周期 -------------------------
    /**
     * @brief 初始化 HAL。应在 app_main 早期、显示之前调用。
     *        M1: 串口/时区/Preferences/按键。后续里程碑追加 WiFi/NTP 等。
     */
    void init();
    /** 周期更新(在按键任务里调用)。M2 起补电压/充电状态采样。 */
    void update();

    // ------------------------- 按键(拨轮三键) -------------------------
    // OneButton(pin, activeLow)。引脚/有效电平来自 starboard_config。
    OneButton btnl = OneButton(PIN_BUTTONL, BUTTON_ACTIVE_LOW);
    OneButton btnc = OneButton(PIN_BUTTONC, BUTTON_ACTIVE_LOW);
    OneButton btnr = OneButton(PIN_BUTTONR, BUTTON_ACTIVE_LOW);
    /** 三个按键各 tick 一次,周期调用(由内部任务保证)。 */
    void tickButtons();
    /** 阻塞到三个按键都松开(LiClock 同名函数,切 App 时用)。 */
    void waitForAllReleased();

    // ------------------------- 配置持久化 -------------------------
    Preferences pref; // NVS 键值存储(namespace "starboard")

    // ------------------------- 时间 -------------------------
    struct tm timeinfo = {};
    time_t now = 0;
    void getTime(); // 读系统时间填 timeinfo/now(系统时间由 esp_netif_sntp 维护)

    // ------------------------- 电压/电源(M2 起实现) -------------------------
    int16_t VCC = 0;          // 电池电压(mV)
    bool USBPluggedIn = false; // USB 已插入
    bool isCharging = false;   // 充电中

    // ------------------------- WiFi + NTP(M3/M4) -------------------------
    enum class WifiState { Idle, Connecting, Connected, Provisioning, Failed };
    WifiState wifiState = WifiState::Idle;
    String wifiSsid;         // 连上后填当前 SSID
    bool timeSynced = false; // NTP 是否已同步
    /** 建栈:NVS/netif/event/默认 STA + 注册 WIFI/IP/SC 事件 + esp_wifi_start,
     *  然后【阻塞等连接结果(超时 timeoutSec 秒)】。连上/超时都返回,不让 app 无限挂住。
     *  已配网(esp_wifi NVS 有凭据)则自动连接,否则进入 SmartConfig(ESPTouch_AirKiss)配网。
     *  超时返回 wifiState=Failed,app 应继续用本地 RTC 时间显示(深睡期间走时维持)。 */
    void wifiInit(uint32_t timeoutSec = 8);
    /** GOT_IP 后启动 esp_netif_sntp 同步。 */
    void ntpStart();

    // ------------------------- 深睡(M5 起实现) -------------------------
    bool wakeUpFromDeepSleep = false;
    /** 上次唤醒的按键 PIN(4/5/6),-1=非按键唤醒(TIMER/正常上电)。M5 验证用。 */
    int wakeupButton = -1;
    /** 进深睡。sec>0 同时启用 timer 唤醒(秒);sec==0 仅按键唤醒。不返回。 */
    void goSleep(uint32_t sec = 0);
    /** 请求深睡标志:按键回调只置位,任务循环在 tick() 返回后的干净栈上再调 goSleep(),
     *  避免在 OneButton::tick 的回调栈里直接跑 esp_sleep(重入 + 深栈)。 */
    volatile bool wantSleep = false;
    uint32_t sleepSec = 0;
    /** 在 init 早期调用:检测唤醒原因,设 wakeUpFromDeepSleep,记录唤醒键。 */
    void checkWakeupCause();

private:
};

extern HAL hal;

#endif // STARBOARD_HAL_H
