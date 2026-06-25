// appWebIDE —— Blockly 可视化编程 Web IDE
//
// 进入后连接 WiFi,启动 Web 服务器,显示设备 IP。
// 用户在同局域网用浏览器打开 http://<设备IP>/ 使用 Blockly。

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>
#include <starboard_display.h>
#include <starboard_gui.h>
#include <lua_webserver.h>
#include <Arduino.h>
#include <WiFi.h>

namespace
{

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
        {
            display.setFullWindow();
            display.firstPage();
            do {
                display.fillScreen(COL_BG);
                u8g2.setFont(CN_FONT_MAIN);
                u8g2.setForegroundColor(COL_NORMAL);
                u8g2.drawUTF8(40, 150, "正在连接 WiFi...");
            } while (display.nextPage());
        }

        hal.wifiInit(10);

        if (hal.wifiState != HAL::WifiState::Connected)
        {
            GUI::msgbox("Web 编程", "WiFi 未连接\n请先配网再使用");
            appManager.goBack();
            return;
        }

        // 启动 Web 服务器
        startBlocklyServer();

        String ipStr = WiFi.localIP().toString();
        String msg = "Blockly 已启动\n\n浏览器打开:\nhttp://" + ipStr + "/\n\n可视化编程,保存即生效";

        GUI::msgbox("Web 编程", msg.c_str());
        appManager.goBack();
    }
};

AppWebIDE appWebIDEInst;

} // namespace

extern AppBase *const appWebIDE = &appWebIDEInst;