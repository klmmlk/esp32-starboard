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
#include <starboard_config.h>  // PIN_BUTTONL/C/R(系统停止检测读三键)
#include <starboard_hal.h>     // hal.pref(读 sleep_to 无操作超时)
#include <starboard_gui.h>     // GUI::setStopCheck/resetStopCheck/pollInputs
#include <starboard_display.h> // display_idleHibernate(屏幕空闲关驱动省电)

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
// 外部调 requestLuaStop() 置标志;lua_execute 内的 line hook(luaSysTick)检测到标志后
// 用 luaL_error 中断脚本(死循环也能跳出)。运行期系统监控(luaSysPollStop)也走此标志。
static volatile bool g_luaStopRequested = false;
static volatile bool g_luaRunning = false;

// ---- Lua 运行期系统监控状态(无操作超时深睡 / 中键长按>1s 退出)----
static volatile unsigned long g_lastActivityMs = 0;      // 最后按键活动时间
static volatile uint32_t      g_sleepToSec = 60;         // 无操作超时(秒,从 pref "sleep_to")
static volatile bool          g_luaSuppressSleep = false;// 豁免超时深睡(Web IDE 在线运行)
static volatile LuaStopReason g_luaStopReason = LUA_STOP_NONE;
static volatile bool          g_cTiming = false;         // 中键长按边沿计时
static volatile unsigned long g_cStart = 0;
static const unsigned long    C_EXIT_HOLD_MS = 1000;     // 中键长按退出阈值(>1s)

void requestLuaStop() {
    // 只在 Lua App 模式(g_luaSuppressSleep=false)生效,Web IDE 模式 suppressSleep=true 跳过。
    if (!g_luaSuppressSleep) g_luaStopRequested = true;
}
bool isLuaRunning() { return g_luaRunning; }
bool luaStopRequested() { return g_luaStopRequested; }

// 轻量停止检查:刷新活动时间 + 检测无操作超时/中键长按,命中则置停止标志+原因。
// 不做 luaL_error(GUI s_stopCheck 回调无 lua_State;由 hook/luaSysTick/绑定层负责跳转)。
bool luaSysPollStop()
{
    display_idleHibernate();   // 屏幕空闲>10s 关驱动省电(不门控 suppressSleep:Web IDE 挂着也要关屏)
    if (!g_luaRunning) return false;
    // 活动刷新:任意键按下 = 用户在操作(digitalRead 物理电平,不消费事件队列)
    if (digitalRead(PIN_BUTTONL) == LOW || digitalRead(PIN_BUTTONC) == LOW || digitalRead(PIN_BUTTONR) == LOW)
        g_lastActivityMs = millis();
    // (1) 无操作超时 → 深睡(Web IDE 模式 suppressSleep 跳过)
    if (!g_luaSuppressSleep && (millis() - g_lastActivityMs > g_sleepToSec * 1000UL))
    {
        g_luaStopReason = LUA_STOP_SLEEP;
        g_luaStopRequested = true;
        return true;
    }
    // (2) 中键长按 >1s → 退出(边沿计时:短按<1s 松开后重置,不误触)
    if (digitalRead(PIN_BUTTONC) == LOW)
    {
        if (!g_cTiming) { g_cTiming = true; g_cStart = millis(); }
        else if (millis() - g_cStart >= C_EXIT_HOLD_MS)
        {
            g_luaStopReason = LUA_STOP_EXIT;
            g_luaStopRequested = true;
            return true;
        }
    }
    else g_cTiming = false;
    return false;
}

// C++ linkage 包装:GUI::setStopCheck 接收 C++ 函数指针,而 luaSysPollStop 是 extern "C"。
static bool luaSysPollStopCpp() { return luaSysPollStop(); }

// 系统 tick:刷新检测,命中则 luaL_error。供 hook / delay / waitKey 等【有 lua_State】的 yield 点。
void luaSysTick(lua_State *L)
{
    luaSysPollStop();
    if (g_luaStopRequested)
    {
        const char *why = (g_luaStopReason == LUA_STOP_SLEEP) ? "stopped: sleep timeout"
                        : (g_luaStopReason == LUA_STOP_EXIT)  ? "stopped: exit by user"
                        : "stopped by system";
        luaL_error(L, why);
    }
}

void luaSysBeginRun(bool suppressSleep)
{
    g_luaStopRequested = false;
    g_luaRunning = true;
    g_luaSuppressSleep = suppressSleep;
    g_luaStopReason = LUA_STOP_NONE;
    g_lastActivityMs = millis();
    g_cTiming = false;
    long s = hal.pref.getInt("sleep_to", 60);
    if (s < 10) s = 10;
    g_sleepToSec = (uint32_t)s;
    GUI::setStopCheck(luaSysPollStopCpp); // GUI 阻塞(menu/msgbox)也被系统监控接管
}

void luaSysEndRun()
{
    GUI::resetStopCheck();
    g_luaRunning = false;
}

LuaStopReason luaSysStopReason() { return g_luaStopReason; }

static void luaStopHook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    // 系统侧检测(无操作超时/中键长按/外部 requestLuaStop),命中设标志 → luaL_error
    luaSysTick(L);
    // 喂狗:LINE hook 每行触发,Lua 紧凑循环会长时间占用任务;每 ~2000 次 yield 让 IDLE 跑
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
    // 分段 delay,每 10ms 做一次系统 tick(无操作超时/中键长按/外部停止,命中 luaL_error)
    // + 轮询按键(长 delay 期间主线程不调 tryGetKey 会丢键,这里主动 poll 入队)
    while (ms > 0)
    {
        luaSysTick(L);
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
    g_luaStopRequested = false; // 清外部残留(运行期 g_luaRunning 由 luaSysBeginRun/End 管)
    // 用 LINE hook:每行 Lua 代码触发一次。COUNT hook 在小循环体+长时间 C 函数
    // (如 gui.waitKey 阻塞)组合下很难凑够指令数,改 LINE 能在 waitKey 返回后立即触发。
    lua_sethook(L, luaStopHook, LUA_MASKLINE, 0);
    int ret = luaL_dostring(L, code);
    lua_sethook(L, nullptr, 0, 0);
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
    g_luaStopRequested = false; // 清外部残留(运行期 g_luaRunning 由 luaSysBeginRun/End 管)
    lua_sethook(L, luaStopHook, LUA_MASKLINE, 0);
    int ret = luaL_dofile(L, filename);
    lua_sethook(L, nullptr, 0, 0);
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