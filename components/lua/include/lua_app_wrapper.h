#ifndef LUA_APP_WRAPPER_H
#define LUA_APP_WRAPPER_H

// =============================================================================
// lua_app_wrapper —— Lua App 包装器
//
// 扫描 /littlefs/apps/ 目录,每个子目录为一个 .app。
// 每个 .app 目录包含:
//   conf.lua  — 元数据(设置全局变量 title, showInList, resumable)
//   main.lua  — App 入口(setup 时执行)
//
// 使用:
//   scanAndRegisterLuaApps();  // 在 registerBuiltinApps 后调用
// =============================================================================

/** 扫描 /littlefs/apps/,将每个子目录包装成 AppBase 注册进 appManager。 */
void scanAndRegisterLuaApps();

#endif // LUA_APP_WRAPPER_H
