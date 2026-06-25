// starboard_lua —— Lua 运行时胶水层
//
// 移植自 LiClock/src/lua/lua_trans.cpp(132 行)。
// 区别:不全局持有一个 lua_State*,每次 openLua 创建并注册模块后返回。
//
// 阶段5a:跳过 fopen 重定向,lua_execute_string 用于测试。

#include "starboard_lua.h"
#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// lua_printf: Lua 底层输出函数(print/debug/warning 使用)。
// 在 ESP-IDF 环境下重定向到标准 printf(输出到 UART)。
void lua_printf(const char *format, ...)
{
    va_list arg;
    va_start(arg, format);
    vprintf(format, arg);
    va_end(arg);
}

// 模块入口(在各自的 .cpp 中实现)
extern "C" int luaopen_display(lua_State *L);
extern "C" int luaopen_hal(lua_State *L);

// ------------------------- 6 个全局函数(移植自 LiClock lua_trans.cpp) -------------------------

static int common_delay(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int ms = luaL_checkinteger(L, 1);
    delay(ms);
    return 0;
}

static int common_digitalRead(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int pin = luaL_checkinteger(L, 1);
    lua_pushinteger(L, digitalRead(pin));
    return 1;
}

static int common_digitalWrite(lua_State *L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L, "参数个数不符");
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    digitalWrite(pin, val);
    return 0;
}

static int common_analogRead(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int pin = luaL_checkinteger(L, 1);
    lua_pushinteger(L, analogRead(pin));
    return 1;
}

static int common_pinMode(lua_State *L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L, "参数个数不符");
    int pin = luaL_checkinteger(L, 1);
    int mode = luaL_checkinteger(L, 2);
    pinMode(pin, mode);
    return 0;
}

// ------------------------- open/close/exec -------------------------

lua_State *openLua()
{
    lua_State *L = luaL_newstate();
    if (!L)
        return nullptr;

    // 注册标准库
    luaL_openlibs(L);

    // 注册全局函数
    lua_pushcfunction(L, common_delay);
    lua_setglobal(L, "delay");
    lua_pushcfunction(L, common_digitalRead);
    lua_setglobal(L, "digitalRead");
    lua_pushcfunction(L, common_digitalWrite);
    lua_setglobal(L, "digitalWrite");
    lua_pushcfunction(L, common_analogRead);
    lua_setglobal(L, "analogRead");
    lua_pushcfunction(L, common_pinMode);
    lua_setglobal(L, "pinMode");

    // 注册自定义模块:require("display") / require("hal")
    luaL_requiref(L, "display", luaopen_display, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "hal", luaopen_hal, 1);
    lua_pop(L, 1);

    Serial.println("[Lua] 初始化完成");
    return L;
}

int lua_execute_string(lua_State *L, const char *code)
{
    if (!L || !code)
        return -1;
    int ret = luaL_dostring(L, code);
    if (ret != LUA_OK)
    {
        Serial.printf("[Lua] 执行错误: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return ret;
}

int lua_execute(lua_State *L, const char *filename)
{
    if (!L || !filename)
        return -1;
    int ret = luaL_dofile(L, filename);
    if (ret != LUA_OK)
    {
        Serial.printf("[Lua] 文件错误(%s): %s\n", filename, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return ret;
}

void closeLua(lua_State *L)
{
    if (L)
        lua_close(L);
}