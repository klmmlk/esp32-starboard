// starboard_hal 实现 —— 见 include/starboard_hal.h
// 参考移植自 LiClock/src/hal.cpp,已剥离 display/peripherals 依赖。
//
// 当前进度: M1(init + 按键) + M5(深睡/按键唤醒) + M3/M4(WiFi SmartConfig 配网 + NTP)。其余成员(电压)后续里程碑补。

#include "starboard_hal.h"
#include <esp_sleep.h>       // esp_deep_sleep_start / esp_sleep_get_wakeup_cause 等
#include <driver/rtc_io.h>   // rtc_gpio_pullup_en(深睡期间维持 RTC IO 上拉)
#include <nvs_flash.h>       // nvs_flash_init(SmartConfig 凭据存 esp_wifi 默认 NVS)
#include <esp_wifi.h>        // esp_wifi_* + wifi_config_t
#include <esp_event.h>       // esp_event_handler_register
#include <esp_smartconfig.h> // esp_smartconfig_*(SmartConfig 配网,esp_wifi 组件)
#include <esp_netif.h>       // esp_netif_create_default_wifi_sta
#include <esp_netif_sntp.h>  // esp_netif_sntp_*(NTP,esp_netif 组件)

// 深睡唤醒计数(存 RTC 慢速内存,跨深睡保留)。M5 验证 RTC 内存保留用。
RTC_DATA_ATTR uint32_t bootCount = 0;

// -----------------------------------------------------------------------------
// 按键事件回调(M1 验证用:串口打印)。后续里程碑会被 App 框架接管。
// -----------------------------------------------------------------------------
static void onBtnLClick() { Serial.println("[HAL] 左键 短按"); }
static void onBtnCClick() { Serial.println("[HAL] 中键 短按"); }
static void onBtnRClick() { Serial.println("[HAL] 右键 短按"); }
static void onBtnLLongPress() { Serial.println("[HAL] 左键 长按 → 请求深睡"); hal.wantSleep = true; }
static void onBtnCLongPress() { Serial.println("[HAL] 中键 长按"); }
static void onBtnRLongPress() { Serial.println("[HAL] 右键 长按"); }

// 周期 tick 三个按键的任务。OneButton 需要被频繁 tick 才能检测点击/长按。
static void task_hal_update(void *)
{
    while (true)
    {
        hal.tickButtons();
        // 在 tick() 返回后的干净栈上执行深睡,不在 OneButton 回调栈里跑 esp_sleep(防重入/深栈)
        if (hal.wantSleep)
        {
            hal.wantSleep = false;
            hal.goSleep(hal.sleepSec);
        }
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
    while (btnl.isPressing() || btnc.isPressing() || btnr.isPressing())
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

    // 检测唤醒原因(M5):区分正常上电 / 按键唤醒 / timer 唤醒
    checkWakeupCause();

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
    xTaskCreate(task_hal_update, "hal_update", 8192, nullptr, 5, nullptr);
    Serial.println("[HAL] 按键轮询任务已启动");

    // WiFi + NTP(M3/M4):建栈。已配网(esp_wifi NVS 有凭据)则自动连接,否则 SmartConfig 配网
    wifiInit();
    Serial.println("[HAL] init 完成。");
}

void HAL::update()
{
    // M2 起补: 电压采样/充电状态。
}

// -----------------------------------------------------------------------------
// 深睡(M5)
// -----------------------------------------------------------------------------
void HAL::checkWakeupCause()
{
    bootCount++;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause)
    {
    case ESP_SLEEP_WAKEUP_EXT1:
    {
        wakeUpFromDeepSleep = true;
        uint64_t status = esp_sleep_get_ext1_wakeup_status();
        if (status & (1ULL << PIN_BUTTONL))      wakeupButton = PIN_BUTTONL;
        else if (status & (1ULL << PIN_BUTTONC)) wakeupButton = PIN_BUTTONC;
        else if (status & (1ULL << PIN_BUTTONR)) wakeupButton = PIN_BUTTONR;
        else                                      wakeupButton = -1;
        Serial.printf("[HAL] 深睡唤醒(EXT1), 唤醒键=GPIO%d, bootCount=%u\n", wakeupButton, (unsigned)bootCount);
        break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
        wakeUpFromDeepSleep = true;
        wakeupButton = -1;
        Serial.printf("[HAL] 深睡唤醒(TIMER 定时), bootCount=%u\n", (unsigned)bootCount);
        break;
    default:
        wakeUpFromDeepSleep = false;
        wakeupButton = -1;
        Serial.printf("[HAL] 正常上电/复位(非深睡唤醒), bootCount=%u\n", (unsigned)bootCount);
        break;
    }
}

void HAL::goSleep(uint32_t sec)
{
    Serial.println("[HAL] 准备进入深睡...");
    waitForAllReleased(); // 等三键都松开,防唤醒瞬间键还按着又被 OneButton 当事件
    // (M3 起:WiFi 已连则在此 esp_wifi_stop())

    // 三个唤醒引脚开 RTC IO 内部上拉(active-low: 空闲靠上拉维持高,按下变低 → ANY_LOW 唤醒)。
    // 注意:OneButton 设的数字 GPIO INPUT_PULLUP 在深睡时随数字域断电失效,
    // 必须另开 RTC IO 上拉并保 RTC_PERIPH 供电,否则引脚浮空、唤醒不可靠。
    for (int p : {PIN_BUTTONL, PIN_BUTTONC, PIN_BUTTONR})
    {
        rtc_gpio_pullup_en((gpio_num_t)p);
        rtc_gpio_pulldown_dis((gpio_num_t)p);
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    uint64_t mask = (1ULL << PIN_BUTTONL) | (1ULL << PIN_BUTTONC) | (1ULL << PIN_BUTTONR);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (sec)
        esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);

    Serial.flush();
    esp_deep_sleep_start(); // 不返回
}

// -----------------------------------------------------------------------------
// WiFi + SmartConfig 配网 + NTP(M3/M4)
// 依据:官方 smart_config 示例 + esp_netif_sntp 文档。
// 配网:SC_TYPE_ESPTOUCH_AIRKISS —— 微信「乐鑫 AirKiss」小程序或 ESPTouch APP。
// 凭据存 esp_wifi 默认 NVS(esp_wifi_get_config 判断已配网)。
// -----------------------------------------------------------------------------
static void ntpSyncCb(struct timeval *tv)
{
    hal.timeSynced = true;
    hal.getTime();
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &hal.timeinfo);
    Serial.printf("[HAL] NTP 已同步,北京时间 %s\n", buf);
}

static void wifiEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
        {
            // 已配网(esp_wifi NVS 有凭据)→ 直连;否则启动 SmartConfig 配网
            wifi_config_t cfg = {};
            esp_wifi_get_config(WIFI_IF_STA, &cfg);
            if (strlen((const char *)cfg.sta.ssid) > 0)
            {
                Serial.printf("[HAL] 已配网,直连 SSID=%s\n", cfg.sta.ssid);
                hal.wifiState = HAL::WifiState::Connecting;
                esp_wifi_connect();
            }
            else
            {
                Serial.println("[HAL] 未配网,启动 SmartConfig(ESPTouch_AirKiss)");
                Serial.println("[HAL] 用微信「乐鑫 AirKiss」小程序 或 ESPTouch APP 配网(仅 2.4G WiFi)");
                esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS);
                smartconfig_start_config_t scfg = SMARTCONFIG_START_CONFIG_DEFAULT();
                esp_smartconfig_start(&scfg);
                hal.wifiState = HAL::WifiState::Provisioning;
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
            hal.wifiState = HAL::WifiState::Idle;
            Serial.println("[HAL] WiFi 断开,重连...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    }
    else if (base == SC_EVENT)
    {
        switch (id)
        {
        case SC_EVENT_GOT_SSID_PSWD:
        {
            smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)data;
            wifi_config_t cfg = {};
            memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
            memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
            Serial.printf("[HAL] SmartConfig 收到:SSID=%s,连接中...\n", cfg.sta.ssid);
            esp_wifi_set_config(WIFI_IF_STA, &cfg);
            hal.wifiState = HAL::WifiState::Connecting;
            esp_wifi_connect();
            break;
        }
        case SC_EVENT_SEND_ACK_DONE:
            Serial.println("[HAL] SmartConfig 完成,停止监听");
            esp_smartconfig_stop();
            break;
        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        hal.wifiState = HAL::WifiState::Connected;
        wifi_config_t cur = {};
        esp_wifi_get_config(WIFI_IF_STA, &cur);
        hal.wifiSsid = (const char *)cur.sta.ssid;
        Serial.printf("[HAL] WiFi 已连接,SSID=%s,启动 NTP\n", hal.wifiSsid.c_str());
        hal.ntpStart();
    }
}

void HAL::wifiInit()
{
    // NVS:SmartConfig 凭据存 esp_wifi 默认 NVS;容忍分区版本变化
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    // netif + 默认事件循环(arduino-esp32 组件可能已建,重复调返回 INVALID_STATE,忽略)
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr);
    esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

void HAL::ntpStart()
{
    // 注:要多 NTP 源需在 sdkconfig 设 CONFIG_LWIP_SNTP_MAX_SERVERS>=2(默认仅 1,servers[] 数组大小)。
    //     阿里云 NTP 国内稳定,单源 + SNTP 周期重试够用;后续要冗余再调大该 Kconfig。
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = ntpSyncCb;
    esp_netif_sntp_init(&cfg);
    esp_netif_sntp_start();
}

void HAL::getTime()
{
    now = time(nullptr);
    localtime_r(&now, &timeinfo);
}

HAL hal;
