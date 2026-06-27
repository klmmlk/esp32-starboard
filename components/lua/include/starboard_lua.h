#ifndef STARBOARD_LUA_H
#define STARBOARD_LUA_H

// =============================================================================
// starboard_lua —— Lua 运行时入口(胶水层)
//
// 移植自 LiClock/src/lua/lua_trans.cpp:
//   - openLua(): 创建 lua_State,注册标准库 + 自定义模块(display/hal)
//   - lua_execute(filename): 从 LittleFS 执行 Lua 脚本
//   - lua_execute_string(code): 从字符串执行 Lua 脚本(测试用,不需 fopen 重定向)
//   - closeLua(): 销毁
//
// 阶段5a:跳过 fopen 重定向,测试脚本用 lua_execute_string 从固件内嵌字符串执行。
// 阶段5b:lua_execute 从 LittleFS 读文件,需先解决 liolib.c 的 fopen 重定向。
// =============================================================================

#ifdef __cplusplus
extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}
#else
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 Lua 状态,注册标准库 + 自定义模块。返回 lua_State,失败返回 NULL。 */
lua_State *openLua();

/** 从字符串执行 Lua 脚本(目前用于测试,不依赖 VFS/fopen)。返回 0=成功。 */
int lua_execute_string(lua_State *L, const char *code);

/**
 * 从文件执行 Lua 脚本(需 fopen 重定向到 VFS 后才可用)。
 * 阶段5a:暂不可用(iolib.c 的 fopen 仅支持 SD/FATFS,不读 LittleFS)。
 */
int lua_execute(lua_State *L, const char *filename);

/** 关闭 Lua 状态。 */
void closeLua(lua_State *L);

/**
 * 请求停止正在运行的 Lua 脚本(由独立监控任务在硬件按键长按时调用)。
 * lua_execute 内部的 count hook 检测到此标志后用 luaL_error 中断脚本。
 */
void requestLuaStop();

/** Lua 脚本当前是否正在执行(lua_execute 期间为 true)。 */
bool isLuaRunning();

/** 是否收到停止请求(供阻塞型 C 绑定如 gui.waitKey 在等待循环里主动检查)。 */
bool luaStopRequested();

/** 设置/获取当前运行的 Lua App 名(数据持久化按 App 名隔离:data.kv 路径用)。 */
void luaSetCurrentApp(const char *name);
const char *luaGetCurrentApp();

#ifdef __cplusplus
}
#endif

#endif // STARBOARD_LUA_H
