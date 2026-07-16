// starboard_display 实现 —— 见 include/starboard_display.h
//
// 阶段2a: display 全局实例(三色屏 GxEPD2_3C)+ u8g2 中文 + 统一全刷初始化。
// display 实例从原 main.cpp 抽出,放此组件唯一一处定义,供 main/gui/app 共用。

#include "starboard_display.h"
#include <Arduino.h>          // Serial(display_init 一次性)
#include <esp_log.h>          // ESP_LOGI:Serial.println USB 未连接(电池运行)时阻塞,诊断改 ESP_LOGI
static const char *const DISP_TAG = "DISP";

// ---- 实例定义(唯一一处)----
// 引脚全部从 starboard_config 取,改引脚只改那个头文件。
StarboardDisplay display(
    GxEPD2_420c_GDEY042Z98(
        /*CS=*/   CONFIG_SPI_CS,
        /*DC=*/    CONFIG_PIN_DC,
        /*RST=*/   CONFIG_PIN_RST,
        /*BUSY=*/  CONFIG_PIN_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2;

// ---- 屏幕空闲休眠状态机 ----
// 电子纸 bistable:关驱动 IC(display.hibernate)不改显示、纯省电。
// ⚠️ hibernate 后 GxEPD2 的 nextPage/refresh 不会自动 reset/_InitDisplay,直接刷会卡死在
//    _waitWhileBusy(屏幕仍 deep sleep,BUSY 不复位)。刷新前必须 display_wakeIfNeeded() 重新 init。
//    display 层只管"距上次刷新时间 + 是否已休眠",唤醒责任在调用方(刷新前)。
static volatile unsigned long g_lastRefreshMs = 0;       // 最后一次刷新(busy)时刻
static volatile bool g_isHibernate = false;             // 当前是否已 hibernate(防重复 + 幂等)
static volatile bool g_hibernatingInProgress = false;   // 闸门:hibernate() 内部 _PowerOff 也会触发
                                                         //   busy callback,挡住以免把"刚进 hibernate"误判成"刚刷新"

void display_init()
{
    // 显式 pinMode 屏幕 SPI 控制脚为 OUTPUT(GxEPD2 内部 digitalWrite 之前需先设,
    // 否则 arduino-esp32 3.x periman 报 "IO x is not set as GPIO")。
    pinMode(CONFIG_SPI_CS, OUTPUT);
    pinMode(CONFIG_PIN_DC, OUTPUT);
    pinMode(CONFIG_PIN_RST, OUTPUT);
    display.init(0);   // 0=禁用 GxEPD2 诊断输出(_waitWhileBusy 里的 Serial.print 会阻塞/丢 TX,曾致看门狗);Serial 已在 hal.init 初始化
    // setRotation 由 appManager.run() 从 NVS 读,screen_orient 设置才生效;这里不设默认
    display.setTextColor(COL_NORMAL);
    display.fillScreen(COL_BG);

    // u8g2 绑定到 display(传引用)。此时 display 全局对象已构造完成,无静态初始化顺序问题。
    u8g2.begin(display);
    u8g2.setFont(CN_FONT_MAIN);
    u8g2.setForegroundColor(COL_NORMAL);
    u8g2.setBackgroundColor(COL_BG);

    g_lastRefreshMs = millis(); // 初始化刷新时间戳,避免冷启动即被误判为空闲休眠
}

void display_deinit()
{
    display.hibernate();   // 刷完进深睡省电(沿用原 main.cpp 行为)
}

// 刷新打戳:每次全刷的 busy callback 调一次,更新"最后刷新时间"并标记已唤醒。
// 注意 hibernate() 内部的 _PowerOff 也会触发 busy callback —— 由 g_hibernatingInProgress
// 闸门挡住,避免把"刚进入 hibernate"误判成"刚刷新"。
void display_notifyRefresh()
{
    if (g_hibernatingInProgress) return; // hibernate 内部的 PowerOff 不算刷新,跳过
    bool wasHibernate = g_isHibernate;
    g_lastRefreshMs = millis();
    g_isHibernate = false;               // 真刷新发生 = 屏已唤醒
    if (wasHibernate) ESP_LOGI(DISP_TAG, "refresh (driver woke up)"); // 诊断:从休眠恢复
}

// 屏幕空闲休眠:距上次刷新超过 idleSec 秒(默认 10s)则 hibernate 关驱动省电。
// 已休眠则幂等返回 false。各长驻循环(保持期/Web IDE/GUI 阻塞/Lua tick)每轮调一次。
bool display_idleHibernate(unsigned long idleSec)
{
    if (g_isHibernate) return false;                       // 已休眠
    if (millis() - g_lastRefreshMs < idleSec * 1000UL) return false;
    g_hibernatingInProgress = true;                        // 挡住下面 hibernate() 内部 PowerOff 的 busy 回调
    display.hibernate();
    g_hibernatingInProgress = false;
    g_isHibernate = true;
    ESP_LOGI(DISP_TAG, "idle hibernate (driver off)");  // 诊断:进入空闲休眠
    return true;
}

// hibernate 后刷新前的唤醒:GxEPD2 的 nextPage/refresh 不会自动 _reset/_InitDisplay,直接刷会卡死在
// _waitWhileBusy(实测保持期按键重画触发看门狗复位)。任何「idleHibernate 之后、刷新之前」的路径
// (setup 重画 / GUI::menu / 弹窗)都要先调本函数重新 init 屏幕。
void display_wakeIfNeeded()
{
    if (!g_isHibernate) return; // 幂等:未休眠直接返回
    pinMode(CONFIG_SPI_CS, OUTPUT);
    pinMode(CONFIG_PIN_DC, OUTPUT);
    pinMode(CONFIG_PIN_RST, OUTPUT);
    display.init(0, false); // re-init(serial=0 禁诊断;initial=false 不清屏):reset+_InitDisplay 唤醒,接下来 setFullWindow/firstPage 重画
    g_isHibernate = false;
    g_lastRefreshMs = millis();
    ESP_LOGI(DISP_TAG, "wake from hibernate (re-init driver)");
}
