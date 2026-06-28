// lua_appmanager —— Lua 绑定: App 导航(gotoApp/goBack/setWakeupSec)
//
// 移植自 LiClock/src/lua/modules/lua_appManager.cpp(49行,3函数)。
// 适配:用 appManager.setWakeupSec 替代 LiClock 的 appManager.nextWakeup 字段。

#include "starboard_lua.h"
#include <starboard_app.h>
#include <Arduino.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int appmgr_gotoApp(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    const char *name = luaL_checkstring(L, 1);
    appManager.gotoApp(name);
    // 通知 Lua 停止:Lua 任务收到请求后在 LINE hook 里 luaL_error 跳出,
    // setup() 收到通知返回后 run() 处理 pendingSwitch 切换到目标 App。
    requestLuaStop();
    return 0;
}

static int appmgr_goBack(lua_State *L)
{
    appManager.goBack();
    requestLuaStop();
    return 0;
}

static int appmgr_setWakeupSec(lua_State *L)
{
    int sec = 0;
    if (lua_gettop(L) >= 1)
        sec = (int)luaL_checkinteger(L, 1);
    appManager.setWakeupSec((uint32_t)sec);
    return 0;
}

static const luaL_Reg _lualib[] = {
    {"gotoApp", appmgr_gotoApp},
    {"goBack", appmgr_goBack},
    {"setWakeupSec", appmgr_setWakeupSec},
    {NULL, NULL},
};

extern "C" int luaopen_appmanager(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}