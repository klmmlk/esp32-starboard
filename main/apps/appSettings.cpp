// appSettings —— 设置(屏幕方向 / NTP 间隔 / 关于)
//
// 阶段3 最小版:GUI::menu 阻塞菜单循环,全程 NVS(hal.pref)持久化,不引入 config.json/LittleFS。
// 回合制语义:setup() 进菜单循环,用户操作完(选"返回" / 长按意图退出)调 goBack() 回上层。
// 按 plan,GUI 弹窗不恢复背景——本 App 退出后由 appManager 切回上层(caller setup 重画整屏)。

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>     // hal.pref(NVS)
#include <starboard_display.h> // display.setRotation / COL_*
#include <starboard_gui.h>     // GUI::menu / msgbox / msgbox_number
#include <Arduino.h>

namespace
{
// NVS 键名(namespace "starboard")。值:0=正常 / 1=反转(180°)。
constexpr const char *KEY_SCREEN_ORIENT = "screen_orient";
// NTP 同步间隔(分钟)。0=禁用自动同步,仅 OOBE 首次对时。实际定时同步消费留到网络/天气阶段。
constexpr const char *KEY_NTP_INTERVAL = "ntp_interval";

class AppSettings : public AppBase
{
public:
    AppSettings()
    {
        name = "settings";
        title = "设置";
        resumable = true;
        showInList = true;
    }

    void setup() override
    {
        for (;;)
        {
            // 主菜单(末项 {nullptr,nullptr} 哨兵)
            static const menu_item mainMenu[] = {
                {nullptr, "屏幕方向"},
                {nullptr, "NTP 间隔(存配置)"},
                {nullptr, "重新配网"},
                {nullptr, "无操作超时"},
                {nullptr, "OTA 升级"},
                {nullptr, "关于"},
                {nullptr, "返回"},
                {nullptr, nullptr},
            };
            int sel = GUI::menu("设置", mainMenu);
            switch (sel)
            {
            case 0: editScreenOrient(); break;
            case 1: editNtpInterval(); break;
            case 2: wifiReprov(); break;
            case 3: editSleepTimeout(); break;
            case 4: return otaUpgrade();  // gotoApp 设 pendingSwitch,return 让 run() 消费
            case 5: aboutBox(); break;
            default: appManager.goBack(); return; // "返回" 或异常索引 → 回上层
            }
        }
    }

private:
    // 屏幕方向:菜单选 正常/反转,写 NVS 并立即 setRotation 刷新生效。
    void editScreenOrient()
    {
        static const menu_item opts[] = {
            {nullptr, "正常"},
            {nullptr, "反转(180°)"},
            {nullptr, nullptr},
        };
        int cur = hal.pref.getUChar(KEY_SCREEN_ORIENT, 0);
        int sel = GUI::menu("屏幕方向", opts);
        if (sel < 0)
            return;
        hal.pref.putUChar(KEY_SCREEN_ORIENT, (uint8_t)sel);
        display.setRotation(sel == 1 ? 2 : 0); // 0=正常, 2=反转180°
        GUI::msgbox("屏幕方向", sel == 1 ? "已设为反转" : "已设为正常");
    }

    // NTP 同步间隔:msgbox_number 输入分钟数(0=禁用),存 NVS。
    void editNtpInterval()
    {
        int cur = hal.pref.getInt(KEY_NTP_INTERVAL, 720); // 默认 720 分钟(12h)
        int v = GUI::msgbox_number("NTP间隔(分钟)", 4, cur);
        if (v < 0)
            v = 0;
        hal.pref.putInt(KEY_NTP_INTERVAL, v);
        char buf[48];
        snprintf(buf, sizeof(buf), v == 0 ? "已禁用自动同步" : "已设为 %d 分钟", v);
        GUI::msgbox("NTP 同步间隔", buf);
    }

    // 无操作超时(秒):屏幕 10 秒后 hibernate,超时后进深睡
    void editSleepTimeout()
    {
        int cur = hal.pref.getInt("sleep_to", 60);
        int v = GUI::msgbox_number("超时(秒)", 4, cur);
        if (v < 10) v = 10; // 最少 10 秒(屏幕 hibernate 基线)
        hal.pref.putInt("sleep_to", v);
        char buf[128];
        snprintf(buf, sizeof(buf), "已设为 %d 秒\n10 秒后屏幕休眠\n超时后芯片深睡", v);
        GUI::msgbox("无操作超时", buf);
    }

    void wifiReprov()
    {
        // 先确认用户意图
        if (GUI::msgbox_yn("重新配网", "将清空 WiFi 配置并进入配网模式\n(微信 AirKiss 小程序配网)\n继续?"))
        {
            GUI::msgbox("配网中", "配网模式已启动\n用微信「乐鑫 AirKiss」小程序\n或 ESPTouch APP 配网\n\n连上后自动返回");
            // 进入配网 —— 阻塞等待连上
            hal.wifiReprov(180);
            if (hal.wifiState == HAL::WifiState::Connected)
            {
                hal.ntpStart(); // 配网成功后立即 NTP 对时
                GUI::msgbox("配网成功", hal.wifiSsid.c_str());
            }
            else
            {
                GUI::msgbox("配网失败", "未配上网,可稍后重试\n主时钟显示本地时间");
            }
        }
    }

    void otaUpgrade()
    {
        appManager.gotoApp("ota");
        // gotoApp 只设了 pendingSwitch,需要 return 让 run() 消费
        // (否则菜单循环继续,App 切不过去)
    }

    void aboutBox()
    {
        GUI::msgbox("关于", "esp32-starboard\nESP32-S3 三色墨水屏\n阶段3 AppManager + OTA");
    }
};
AppSettings appSettingsInst;
} // namespace

// extern:C++ namespace 作用域 const 变量默认内部链接,须 extern 才能被 apps.cpp 链接到。
extern AppBase *const appSettings = &appSettingsInst;
