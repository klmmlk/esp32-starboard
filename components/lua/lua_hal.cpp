// lua_hal —— Lua 绑定: 硬件抽象层(时间/电源/系统)
//
// 移植自 LiClock/src/lua/modules/lua_hal.cpp(99行,10函数)。
// 适配:跳过 autoConnectWiFi(我们 Wi-Fi 按需初始化);
//      跳过 powerOff(本工程无此函数);
//      跳过 detachAllButtonEvents(本工程不暴露此 API)。

#include "starboard_lua.h"
#include <starboard_hal.h>
#include <Arduino.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------------------- 电源(阶段1 已留字段但无 ADC 采样,返回占位值) ----------------------

static int hal_VCC(lua_State *L)
{
    lua_pushinteger(L, hal.VCC);
    return 1;
}

static int hal_USBPluggedIn(lua_State *L)
{
    lua_pushboolean(L, hal.USBPluggedIn);
    return 1;
}

static int hal_isCharging(lua_State *L)
{
    lua_pushboolean(L, hal.isCharging);
    return 1;
}

// ---------------------- 时间 ----------------------

static int hal_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hal.now);
    return 1;
}

static int hal_timeinfo(lua_State *L)
{
    hal.getTime(); // 先刷新,保证读到最新时间
    // 返回 7 个值: year, month(1-based), day, wday(0=Sun), hour, min, sec
    lua_pushinteger(L, hal.timeinfo.tm_year + 1900);
    lua_pushinteger(L, hal.timeinfo.tm_mon + 1);
    lua_pushinteger(L, hal.timeinfo.tm_mday);
    lua_pushinteger(L, hal.timeinfo.tm_wday);
    lua_pushinteger(L, hal.timeinfo.tm_hour);
    lua_pushinteger(L, hal.timeinfo.tm_min);
    lua_pushinteger(L, hal.timeinfo.tm_sec);
    return 7;
}

// 按字段取时间(field: year/month/day/hour/min/sec/wday),内部自动刷新
static int hal_timeField(lua_State *L)
{
    hal.getTime();
    const char *f = luaL_checkstring(L, 1);
    int v = 0;
    if (strcmp(f, "year") == 0) v = hal.timeinfo.tm_year + 1900;
    else if (strcmp(f, "month") == 0) v = hal.timeinfo.tm_mon + 1;
    else if (strcmp(f, "day") == 0) v = hal.timeinfo.tm_mday;
    else if (strcmp(f, "hour") == 0) v = hal.timeinfo.tm_hour;
    else if (strcmp(f, "min") == 0) v = hal.timeinfo.tm_min;
    else if (strcmp(f, "sec") == 0) v = hal.timeinfo.tm_sec;
    else if (strcmp(f, "wday") == 0) v = hal.timeinfo.tm_wday;
    lua_pushinteger(L, v);
    return 1;
}

static int hal_getTime(lua_State *L)
{
    hal.getTime();
    return 0;
}

// 开机以来的毫秒数(用于空闲超时判断)
static int hal_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

// ---------------------- 系统控制 ----------------------

static int hal_reboot(lua_State *L)
{
    ESP.restart();
    return 0;
}

// ---------------------- 注册表 ----------------------

static const luaL_Reg _lualib[] = {
    {"VCC", hal_VCC},
    {"USBPluggedIn", hal_USBPluggedIn},
    {"isCharging", hal_isCharging},
    {"now", hal_now},
    {"timeinfo", hal_timeinfo},
    {"timeField", hal_timeField},
    {"getTime", hal_getTime},
    {"millis", hal_millis},
    {"reboot", hal_reboot},
    {NULL, NULL},
};

extern "C" int luaopen_hal(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}