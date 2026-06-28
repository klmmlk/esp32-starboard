// lua_sys —— Lua 绑定: 系统控制(sys.yield)
//
// 历史:曾用 sys.yield() 让 AppManager 检测睡眠计时器——实际无效(主任务在 ulTaskNotifyTake
// 挂起,delay(1) 推进不了保持期计时)。现 Lua App 休眠/退出改由 starboard_lua 的 LINE hook +
// luaSysTick 统一接管(见 docs/LUA_APP_SLEEP.md)。sys.yield 保留供旧脚本兼容,作为额外停止检查点。

#include "starboard_lua.h"
#include <Arduino.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int sys_yield(lua_State *L)
{
    luaSysTick(L); // 兼容旧脚本:作为系统停止检查点(中键长按/超时命中则 luaL_error)
    return 0;
}

// ---------------------- 注册表 ----------------------

static const luaL_Reg _lualib[] = {
    {"yield", sys_yield},
    {NULL, NULL},
};

extern "C" int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}
