// appOOBE —— 首次开机引导(欢迎 → SmartConfig 配网 → NTP → 进主时钟)
//
// 阶段3 最小版:用 pref "oobe" 计数(0/1/2/3)记进度。一次回合内顺序跑完所有 stage。
// 配网轮询上限 PROVISION_TIMEOUT_SEC(30s):连不上就进离线主时钟,不干等(避免卡死/耗电)。
// resumable=false:引导完成不记 lastAppName,直接 gotoApp("clock") 链式进主时钟。
// showInList=false:不出现在 App 列表(仅开机据 oobe 进度由 begin() 选为 home)。
//
// 配网失败不卡死:提示后强制 oobe=3 进离线主时钟(显示 --:--),下次 erase-flash 才重试。

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>     // hal.wifiInit / wifiState / pref
#include <starboard_display.h> // display / u8g2 / COL_*
#include <starboard_gui.h>     // GUI::msgbox
#include <esp_smartconfig.h>   // esp_smartconfig_stop(配网超时后停 sniffer)
#include <Arduino.h>

namespace
{
// SmartConfig 配网等待上限(秒)。给用户推 WiFi 的时间,但不再干等5分钟(耗电+卡死);
// 超时仍没连上就进离线主时钟,配网可后续在设置里重新发起(wifiReprov)。
constexpr uint32_t PROVISION_TIMEOUT_SEC = 30;

// 画一帧占满屏的纯文字引导画面(不走 GUI 弹窗)。最多 4 行,第 3 行用红色强调。
void drawGuide(const char *l1, const char *l2 = nullptr, const char *l3Alert = nullptr, const char *l4 = nullptr)
{
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(COL_BG);
        u8g2.setFont(CN_FONT_MAIN);
        int y = 90;
        auto line = [&](const char *s, uint16_t col) {
            if (s) { u8g2.setForegroundColor(col); u8g2.drawUTF8(60, y, s); y += 40; }
        };
        line(l1, COL_NORMAL);
        line(l2, COL_NORMAL);
        line(l3Alert, COL_ALERT);
        line(l4, COL_NORMAL);
    } while (display.nextPage());
}

class AppOOBE : public AppBase
{
public:
    AppOOBE()
    {
        name = "oobe";
        title = "引导";
        showInList = false;
        resumable = false;
    }

    void setup() override
    {
        // stage 0:欢迎
        if (hal.pref.getInt("oobe", 0) < 1)
        {
            GUI::msgbox("欢迎", "esp32-starboard\n首次开机引导\n按任意键继续");
            hal.pref.putInt("oobe", 1);
        }

        // stage 1:配网 + NTP(已配网直连,否则 SmartConfig)
        if (hal.pref.getInt("oobe", 0) < 2)
        {
            drawGuide("正在配网...", "微信「乐鑫 AirKiss」", "或 ESPTouch 推 WiFi(仅2.4G)");
            hal.wifiInit(); // 阻塞:已配网直连,否则进 SmartConfig(Provisioning),8s 超时返回
            // wifiInit 返回后最多再等 PROVISION_TIMEOUT_SEC 秒;连不上就放弃进离线(不再干等5分钟)。
            uint32_t waited = 0;
            while (hal.wifiState != HAL::WifiState::Connected && waited < PROVISION_TIMEOUT_SEC)
            {
                delay(1000);
                ++waited;
            }
            if (hal.wifiState == HAL::WifiState::Connected)
            {
                hal.pref.putInt("oobe", 2);
                drawGuide("配网成功", "正在同步时间...");
                delay(2000); // 给 SNTP(异步)一点时间出回调
            }
            else
            {
                esp_smartconfig_stop(); // 若 SmartConfig sniffer 仍在跑,停掉避免持续耗电/干扰
                GUI::msgbox("配网", "未连上 WiFi\n进入离线模式(时间显示 --:--)\n可在设置中重新配网");
                // 不递增到 2,但下方 stage 2 会把 oobe 置 3,避免每次开机重卡 OOBE。
            }
        }

        // stage 2:完成 → 进主时钟
        hal.pref.putInt("oobe", 3);
        GUI::msgbox("完成", "引导完成\n进入主时钟");
        appManager.gotoApp("clock"); // 回合内链式切到主时钟(继续跑 clock.setup)
    }
};
AppOOBE appOOBEInst;
} // namespace

// extern:C++ namespace 作用域 const 变量默认内部链接,须 extern 才能被 apps.cpp 链接到。
extern AppBase *const appOOBE = &appOOBEInst;
