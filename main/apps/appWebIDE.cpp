// appWebIDE —— Blockly 可视化编程 Web IDE
//
// 进入后连接 WiFi → 启动 Web 服务器 → 进入轮询循环持续处理请求
// 用户同局域网用浏览器打开 http://<设备IP>/ 使用 Blockly
// 按中键退出返回 App 列表

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>
#include <starboard_display.h>
#include <starboard_gui.h>
#include <lua_webserver.h>
#include <starboard_config.h>
#include <Arduino.h>

namespace
{

void drawStatus(const char *line1, const char *line2 = nullptr,
                const char *line3 = nullptr, const char *line4 = nullptr)
{
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(COL_BG);
        u8g2.setFont(CN_FONT_MAIN);
        int y = 80;
        auto line = [&](const char *s, uint16_t col) {
            if (s) { u8g2.setForegroundColor(col); u8g2.drawUTF8(30, y, s); y += 36; }
        };
        line(line1, COL_NORMAL);
        line(line2, COL_NORMAL);
        line(line3, COL_ALERT);
        line(line4, COL_NORMAL);
    } while (display.nextPage());
}

class AppWebIDE : public AppBase
{
public:
    AppWebIDE()
    {
        name = "webide";
        title = "Web 编程";
        resumable = false;
        showInList = true;
    }

    void setup() override
    {
        // 连接 WiFi
        drawStatus("正在连接 WiFi...");
        hal.wifiInit(10);

        if (hal.wifiState != HAL::WifiState::Connected)
        {
            GUI::msgbox("Web 编程", "WiFi 未连接\n请先配网再使用");
            appManager.goBack();
            return;
        }

        // 启动 Web 服务器
        startBlocklyServer();

        // 进入轮询循环,同时服务 Web 请求
        drawStatus("Web IDE 已启动", hal.wifiIp.c_str(),
                   "浏览器打开此地址", "按中键退出");

        unsigned long startMs = millis();
        const unsigned long TIMEOUT_MS = 30 * 60 * 1000UL; // 30 分钟超时

        while (true)
        {
            // 处理 Web 请求
            handleBlocklyClient();

            // 超时检查
            if (millis() - startMs > TIMEOUT_MS)
            {
                drawStatus("Web IDE 超时", "即将返回");
                delay(1000);
                break;
            }

            // 中键退出
            if (digitalRead(PIN_BUTTONC) == LOW)
            {
                delay(50);
                if (digitalRead(PIN_BUTTONC) == LOW)
                {
                    drawStatus("已退出", "返回 App 列表");
                    delay(1000);
                    break;
                }
            }

            delay(50);
        }

        appManager.goBack();
    }
};

AppWebIDE appWebIDEInst;

} // namespace

extern AppBase *const appWebIDE = &appWebIDEInst;