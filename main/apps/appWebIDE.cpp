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
#include <lua_app_wrapper.h> // syncLuaApps(Web IDE 增删 App 后同步注册表)
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

        const unsigned long IDLE_TIMEOUT_SEC = 10 * 60; // 无连接 10 分钟超时

        while (true)
        {
            // 处理 Web 请求(有请求时会刷新活动时间)
            handleBlocklyClient();

            // Web IDE 保存/删除了 App → 主线程增量同步(新建的注册、删除的注销),
            // 这样退出 Web IDE 后应用列表立即反映最新,无需重启开发板
            if (appsDirty())
            {
                clearAppsDirty();
                syncLuaApps();
            }

            // 主线程检测并执行待运行的 App(/api/run 设的标志)
            // 在主线程跑 Lua+display,避免和 webserver 线程冲突导致残影/重绘
            if (pollRunRequest())
            {
                // Lua 刚执行完。若被「中键长按3秒」强停,此时用户可能仍按住中键,
                // 直接进入下方的中键退出检测会误触退出 → 先等中键释放再继续。
                while (digitalRead(PIN_BUTTONC) == LOW)
                    delay(20);
                // Lua 若调了 gotoApp(设了 pendingSwitch),退出 Web IDE,让 appManager
                // 主循环消费切换——否则 web IDE 占着主线程,切换请求永远不生效。
                if (appManager.hasPendingSwitch())
                    break;
            }

            // 空闲超时:长时间无客户端访问才退出(有活跃连接永不超时)
            if (blocklyServerIdleTimeout(IDLE_TIMEOUT_SEC))
            {
                drawStatus("Web IDE 空闲超时", "即将返回");
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

        // 退出 Web IDE:若 Lua 已设 gotoApp 目标,则不 goBack(避免覆盖),让 run 切到目标 App;
        // 否则正常返回上层。
        if (!appManager.hasPendingSwitch())
            appManager.goBack();
    }
};

AppWebIDE appWebIDEInst;

} // namespace

extern AppBase *const appWebIDE = &appWebIDEInst;