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
#include <starboard_display.h> // display.hibernate(深睡前关屏幕驱动电源)

// 深睡唤醒计数(存 RTC 慢速内存,跨深睡保留)。M5 验证 RTC 内存保留用。
RTC_DATA_ATTR uint32_t bootCount = 0;

// WiFi 重连退避:连不上时按失败次数递增间隔重连,避免疯狂重连刷屏/耗电。
// (esp_wifi 内部已有一定重试,这里在断开事件上再加一层节流。)
static constexpr uint8_t WIFI_RECONNECT_MAX_FAIL = 6; // 达此失败数后停重连(等下次唤醒重试)
static uint8_t wifiReconnectFail = 0;                 // 当前连续失败次数

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
        if (!hal.pauseButtons) // GUI/appManager 阻塞交互期间暂停 tick(防按键回调打断)
        {
            hal.tickButtons();
            // 深睡已改由 appManager.deepSleep() 在 run() 末尾的干净栈上调用(回合制),
            // 不再用按键回调置 wantSleep 标志的方式。
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

    // NVS(pref 配置 + esp_wifi 凭据都需要)。阶段3 起 WiFi 按需联,但 NVS 在 init 就绪。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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

    // WiFi/NTP 不在 init 联网(阶段3 起【按需】:OOBE 配网 / 天气 App 才调 hal.wifiInit)。
    // 主时钟靠深睡期间 RTC 维持走时;首次上电未对时显示 --:--,由 OOBE 配网 + NTP 校准。
    Serial.println("[HAL] init 完成(WiFi 按需,未联网)。");
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
    display.hibernate(); // 屏幕驱动进入低功耗(关 DC-DC),E-ink 双稳态内容保留
    waitForAllReleased(); // 等三键都松开,防唤醒瞬间键还按着又被 OneButton 当事件
    // hibernate 后屏幕不耗电;深睡后 ESP32 也断电,全机微安级待机
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

// STA_START 事件里是否允许自动启动 SmartConfig。
// wifiInit(首次/开机配网)走 STA_START 自动分支;wifiReprov 自己显式启 SmartConfig,
// 要抑制自动分支(否则会启两个 SmartConfig,第二个返回 -1 ESP_ERR_WIFI_CONN)。
static bool allowAutoSmartconfig = true;

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
            else if (allowAutoSmartconfig)
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
        {
            hal.wifiState = HAL::WifiState::Idle;
            // 退避重连:失败次数递增到上限就停,不再疯狂 esp_wifi_connect 刷屏。
            // reason 见 WIFI_REASON_*;密码错/找不到 AP 会持续失败,退避后等下次唤醒再试。
            if (wifiReconnectFail < WIFI_RECONNECT_MAX_FAIL)
            {
                wifiReconnectFail++;
                uint32_t backoff = 500UL * wifiReconnectFail; // 0.5s,1s,1.5s...线性退避
                Serial.printf("[HAL] WiFi 断开,退避 %lums 后重连(第 %u 次)...\n",
                              (unsigned long)backoff, wifiReconnectFail);
                vTaskDelay(pdMS_TO_TICKS(backoff));
                esp_wifi_connect();
            }
            else if (wifiReconnectFail == WIFI_RECONNECT_MAX_FAIL)
            {
                wifiReconnectFail++; // 防重复打印(只在到上限时打一次)
                Serial.println("[HAL] WiFi 重连已达上限,停止重连(等下次唤醒重试)。");
                hal.wifiState = HAL::WifiState::Failed;
            }
            break;
        }
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
        wifiReconnectFail = 0; // 连上,清零失败计数
        wifi_config_t cur = {};
        esp_wifi_get_config(WIFI_IF_STA, &cur);
        hal.wifiSsid = (const char *)cur.sta.ssid;
        Serial.printf("[HAL] WiFi 已连接,SSID=%s,启动 NTP\n", hal.wifiSsid.c_str());
        hal.ntpStart();
    }
}

// STA_START 自动 SmartConfig 开关 allowAutoSmartconfig 定义见上方(wifiEventHandler 前)。

// 内部:确保 WiFi 驱动已初始化(NVS/netif/event/handler/wifi_init/start 幂等)。
// 重新配网入口来自不联网的主时钟时,WiFi 可能从未初始化,必须先调这个。
static bool wifiDrvInited = false;
static void wifiEnsureInit()
{
    if (wifiDrvInited)
        return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    esp_netif_init();                     // 已建返回 INVALID_STATE,忽略
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr);
    esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifiDrvInited = true;
    Serial.println("[HAL] WiFi 驱动已初始化");
}

void HAL::wifiInit(uint32_t timeoutSec)
{
    wifiEnsureInit(); // NVS/netif/event/wifi_init/start(幂等)

    // 【阻塞等连接结果,超时即放弃】避免"连不上旧 WiFi → 无限挂死 app"。
    // 连上(Connected)/配网中(Provisioning)即返回;超时设 Failed 让 app 用本地 RTC 时间继续。
    Serial.printf("[HAL] 等待 WiFi 连接(超时 %lus)...\n", (unsigned long)timeoutSec);
    uint32_t waited = 0;
    const uint32_t stepMs = 200;
    while (wifiState != WifiState::Connected && wifiState != WifiState::Provisioning
           && waited < timeoutSec * 1000)
    {
        delay(stepMs);
        waited += stepMs;
    }
    if (wifiState != WifiState::Connected && wifiState != WifiState::Provisioning)
    {
        wifiState = WifiState::Failed;
        Serial.println("[HAL] WiFi 连接超时,放弃(继续用本地 RTC 时间显示)。");
        // 停掉反复重连刷屏:让退避计数停在最大,不再立刻 esp_wifi_connect。
        wifiReconnectFail = WIFI_RECONNECT_MAX_FAIL;
    }
}

void HAL::wifiReprov(uint32_t timeoutSec)
{
    Serial.println("[HAL] 重新配网:清旧配置 + 重启WiFi → STA_START 自动启 SmartConfig");

    // 临时关闭 STA_START 自动 SC 分支:否则下面 wifiEnsureInit 的 esp_wifi_start() 会
    // 先启一个 SC,随后我们 stop+start 再启第二个,两个 SC 冲突报 "smartconfig busy"。
    allowAutoSmartconfig = false;

    // 重新配网入口来自不联网的主时钟,WiFi 驱动可能从未初始化。先确保就绪(此轮 start 不启 SC)。
    wifiEnsureInit();

    // 停旧 SmartConfig(若历史启过)+ 断开旧连接
    esp_smartconfig_stop();
    esp_wifi_disconnect();
    delay(100);

    // 清当前配置(写空)+ 擦 NVS 凭据 → 重启后 STA_START 检测配置空 → 自动启 SmartConfig
    wifi_config_t empty = {};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_wifi_restore();
    delay(50);

    // 现在开启自动 SC 分支:下面重启 WiFi 的 STA_START 会走它(唯一的 SC)。
    allowAutoSmartconfig = true;
    wifiState = WifiState::Provisioning;
    wifiReconnectFail = 0;

    // 重启 WiFi(stop→start)触发新的 WIFI_EVENT_STA_START → 自动 SmartConfig 分支(唯一一次)。
    Serial.println("[HAL] 重启 WiFi,等待 STA_START 自动启动 SmartConfig...");
    Serial.println("[HAL] 用微信「乐鑫 AirKiss」小程序或 ESPTouch APP 配网(仅 2.4G WiFi)");
    esp_wifi_stop();
    delay(100);
    // ⚠️ esp_wifi_restore() 会把 WiFi 模式重置成默认(softAP)!SmartConfig 的 sniffer
    //    必须在 STA 模式才能工作(errno 12293 sc_sniffer.c)。start 前务必重设回 STA。
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    // 阻塞等连接结果
    uint32_t waited = 0;
    while (waited < timeoutSec * 1000 && wifiState != WifiState::Connected)
    {
        delay(200);
        waited += 200;
        if (wifiState == WifiState::Failed) break;
    }
    if (wifiState == WifiState::Connected)
    {
        Serial.println("[HAL] 重新配网成功,停止 SmartConfig");
        esp_smartconfig_stop();
        wifi_config_t cur = {};
        esp_wifi_get_config(WIFI_IF_STA, &cur);
        wifiSsid = (const char *)cur.sta.ssid;
        Serial.printf("[HAL] SSID=%s\n", wifiSsid.c_str());
    }
    else
    {
        wifiState = WifiState::Failed;
        Serial.println("[HAL] 重新配网超时/失败");
    }
}

void HAL::ntpStart()
{
    // 注:要多 NTP 源需在 sdkconfig 设 CONFIG_LWIP_SNTP_MAX_SERVERS>=2(默认仅 1,servers[] 数组大小)。
    //     阿里云 NTP 国内稳定,单源 + SNTP 周期重试够用;后续要冗余再调大该 Kconfig。
    static bool ntpInited = false;
    if (ntpInited)
        return; // 防重复:OOBE 已对时过,重新配网后再调会报 "esp_netif_sntp already initialized"
    ntpInited = true;
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
