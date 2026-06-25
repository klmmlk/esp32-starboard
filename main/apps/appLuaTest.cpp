// appLuaTest —— Lua 运行时验证 App
//
// 阶段5a 测试:用 lua_execute_string 执行一段内嵌 Lua 脚本,
// 验证 Lua 运行时 + display 模块 + hal 模块能正常调用。
// 不依赖 fopen/LittleFS,脚本写在固件代码中。
//
// 回合制语义:setup() 执行 Lua 脚本,完成后返回(goBack)。

#include "apps.h"
#include <starboard_app.h>
#include <starboard_hal.h>
#include <starboard_display.h>
#include <starboard_gui.h>
#include <starboard_lua.h>
#include <Arduino.h>

namespace
{

class AppLuaTest : public AppBase
{
public:
    AppLuaTest()
    {
        name = "luatest";
        title = "Lua 测试";
        resumable = false;
        showInList = true;
    }

    void setup() override
    {
        // 1. 初始化 Lua 状态
        lua_State *L = openLua();
        if (!L)
        {
            GUI::msgbox("Lua 测试", "Lua 初始化失败");
            appManager.goBack();
            return;
        }

        // 2. 执行测试脚本
        const char *script =
            "-- Lua 测试脚本(GxEPD2 分页模式)\n"
            "\n"
            "-- 1. 开始一帧(调用 firstPage,之后绘制进入缓冲区)\n"
            "display.beginFrame()\n"
            "\n"
            "-- 2. 绘制内容(在缓冲区中)\n"
            "display.fillScreen(1)  -- 白色背景\n"
            "display.drawRect(10, 10, 380, 280, 0)  -- 黑色边框\n"
            "display.fillCircle(200, 150, 60, 0)  -- 黑色填充圆\n"
            "\n"
            "-- 3. 文字\n"
            "display.setCursor(60, 220)\n"
            "display.setTextColor(0)\n"
            "display.u8g2Print('Hello from Lua!')\n"
            "\n"
            "-- 4. 结束一帧(nextPage 发送到屏幕)\n"
            "display.endFrame()\n"
            "\n"
            "-- 5. 获取时间\n"
            "hal.getTime()\n"
            "local y, m, d, wd, h, min, s = hal.timeinfo()\n"
            "print('Time: ' .. h .. ':' .. min .. ':' .. s)\n"
            "\n"
            "print('Lua script OK!')\n";

        int ret = lua_execute_string(L, script);

        // 3. 全刷使显示生效
        display.setFullWindow();
        display.firstPage();
        do { } while (display.nextPage());

        // 4. 显示结果
        if (ret == 0)
        {
            GUI::msgbox("Lua 测试", "脚本执行成功\n串口有 print 输出");
        }
        else
        {
            GUI::msgbox("Lua 测试", "脚本执行失败\n请查看串口输出");
        }

        // 5. 清理
        closeLua(L);
        appManager.goBack();
    }
};

AppLuaTest appLuaTestInst;

} // namespace

extern AppBase *const appLuaTest = &appLuaTestInst;