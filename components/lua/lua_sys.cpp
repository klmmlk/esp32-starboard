// lua_sys —— Lua 绑定: 系统控制(放权/睡眠)
//
// sys.yield() 让出 CPU 1ms,让 AppManager 在 Lua App 运行期间也能检测睡眠计时器。
// Lua App 应在 while 循环里定期调用 sys.yield(),防止阻塞系统事件处理。

#include "starboard_lua.h"
#include <Arduino.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int sys_yield(lua_State *L)
{
    delay(1); // 让出 CPU 1ms,AppManager 保持期计时器得以推进
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
