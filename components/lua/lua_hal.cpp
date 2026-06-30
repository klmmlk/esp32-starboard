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

// ---------------------- WiFi（按需连接） ----------------------
// 回合制深睡唤醒后 WiFi 未初始化（hal.init 不自动联网，lwIP 协议栈没起），
// http.get 前必须先调本函数连 WiFi，否则一解析地址就 assert "Invalid mbox" 崩溃。
// timeoutSec: 等连接的秒数（默认 8）；返回 true=已连上、false=超时未连上。
static int hal_wifiConnect(lua_State *L)
{
    uint32_t timeoutSec = (lua_gettop(L) >= 1) ? (uint32_t)luaL_checkinteger(L, 1) : 8;
    hal.wifiInit(timeoutSec);
    lua_pushboolean(L, hal.wifiState == HAL::WifiState::Connected);
    return 1;
}

// 唤醒键:本次(深睡)唤醒是由哪个按键触发。返回 1=左 2=中 3=右,0=非按键唤醒(定时/冷启动)。
// 用途:按键唤醒的物理"按下"发生在 App 跑起来之前,gui.waitKey 捕获不到那个边沿(事件丢失),
//      故 Lua App 在 setup 开头用本函数显式读取唤醒键并自行响应,而非依赖 waitKey 第一帧。
// ★ 消费式:读完即清零(wakeupButton=-1)。同回合内第一次调用返回本次唤醒键,之后再读返回 0
//   ——避免 while 循环里反复读到同一唤醒键、重复触发动作。下次深睡唤醒由 checkWakeupCause 重设。
//   框架的中键手势判断在 setup 之前已读完,不受此清零影响。
static int hal_wakeupKey(lua_State *L)
{
    int wb = hal.wakeupButton;
    hal.wakeupButton = -1;  // 读后清零(消费式):同回合内只返回一次唤醒键
    if (wb == PIN_BUTTONL)      lua_pushinteger(L, 1);
    else if (wb == PIN_BUTTONC) lua_pushinteger(L, 2);
    else if (wb == PIN_BUTTONR) lua_pushinteger(L, 3);
    else                        lua_pushinteger(L, 0);
    return 1;
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
    {"wifiConnect", hal_wifiConnect},
    {"wakeupKey", hal_wakeupKey},
    {NULL, NULL},
};

extern "C" int luaopen_hal(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}