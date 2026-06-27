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
#include <starboard_gui.h>   // GUI::pollInputs (delay 期间轮询按键)

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

// ---- 强制停止 Lua 的机制 ----
// 独立监控任务(检测硬件按键长按)调 requestLuaStop() 置标志;
// lua_execute 内的 line hook 检测到标志后用 luaL_error 中断脚本(死循环也能跳出)。
static volatile bool g_luaStopRequested = false;
static volatile bool g_luaRunning = false;

void requestLuaStop() { g_luaStopRequested = true; }
bool isLuaRunning() { return g_luaRunning; }
bool luaStopRequested() { return g_luaStopRequested; }

static void luaStopHook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (g_luaStopRequested)
        luaL_error(L, "stopped by user (长按中键强制停止)");
    // 定期让出 CPU 给 IDLE 任务喂看门狗(LINE hook 每行触发,Lua 紧凑循环
    // 可能长时间占用 main 任务导致 task_wdt 触发)。每 ~2000 次 hook yield 一次。
    static uint16_t yieldCnt = 0;
    if (++yieldCnt >= 2000)
    {
        yieldCnt = 0;
        vTaskDelay(1); // 1 tick (~10ms 以下) 让 IDLE 跑
    }
}

// 当前运行的 Lua App 名(数据持久化按 App 隔离)
static char g_currentLuaApp[64] = {0};
void luaSetCurrentApp(const char *name)
{
    if (!name) { g_currentLuaApp[0] = 0; return; }
    strncpy(g_currentLuaApp, name, sizeof(g_currentLuaApp) - 1);
    g_currentLuaApp[sizeof(g_currentLuaApp) - 1] = 0;
}
const char *luaGetCurrentApp() { return g_currentLuaApp; }

// 模块入口(在各自的 .cpp 中实现)
extern "C" int luaopen_display(lua_State *L);
extern "C" int luaopen_hal(lua_State *L);

// ------------------------- 6 个全局函数(移植自 LiClock lua_trans.cpp) -------------------------

static int common_delay(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int ms = luaL_checkinteger(L, 1);
    // 分段 delay,每 10ms 检查停止标志 + 轮询按键
    // (长 delay 期间主线程不调 tryGetKey 会丢键,这里主动 poll 入队)
    while (ms > 0)
    {
        if (g_luaStopRequested)
            luaL_error(L, "stopped by user (长按中键)");
        GUI::pollInputs();
        int step = ms > 10 ? 10 : ms;
        delay(step);
        ms -= step;
    }
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
    g_luaStopRequested = false;
    g_luaRunning = true;
    // 用 LINE hook:每行 Lua 代码触发一次。COUNT hook 在小循环体+长时间 C 函数
    // (如 gui.waitKey 阻塞)组合下很难凑够指令数,改 LINE 能在 waitKey 返回后立即触发。
    lua_sethook(L, luaStopHook, LUA_MASKLINE, 0);
    int ret = luaL_dostring(L, code);
    lua_sethook(L, nullptr, 0, 0);
    g_luaRunning = false;
    if (ret != LUA_OK)
    {
        if (g_luaStopRequested)
            Serial.println("[Lua] 脚本被强制停止");
        else
            Serial.printf("[Lua] 执行错误: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    g_luaStopRequested = false;
    return ret;
}

int lua_execute(lua_State *L, const char *filename)
{
    if (!L || !filename)
        return -1;
    g_luaStopRequested = false;
    g_luaRunning = true;
    lua_sethook(L, luaStopHook, LUA_MASKLINE, 0);
    int ret = luaL_dofile(L, filename);
    lua_sethook(L, nullptr, 0, 0);
    g_luaRunning = false;
    if (ret != LUA_OK)
    {
        if (g_luaStopRequested)
            Serial.println("[Lua] 脚本被强制停止");
        else
            Serial.printf("[Lua] 文件错误(%s): %s\n", filename, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    g_luaStopRequested = false;
    return ret;
}

void closeLua(lua_State *L)
{
    if (L)
        lua_close(L);
}