#ifndef STARBOARD_APPS_H
#define STARBOARD_APPS_H

// =============================================================================
// 内置 App 实例聚合 —— 每个 appXxx.cpp 定义具体子类,并以基类指针暴露一个全局实例。
// main 在 appManager.run() 前调 registerBuiltinApps() 集中注册 + 设 home。
// 用基类指针(AppBase*)而非具体类型,避免本头依赖 starboard_app.h(解耦)。
// =============================================================================

class AppBase;

extern AppBase *const appClock;    // 主时钟(默认 home)
extern AppBase *const appSettings; // 设置
extern AppBase *const appOOBE;     // 首次开机引导(showInList=false,仅 begin 据 oobe 选 home)
extern AppBase *const appOTA;      // OTA 空中升级
extern AppBase *const appWebIDE;   // Blockly 可视化编程

/** 注册全部内置 App 进 appManager + 设 home。main 在 appManager.run() 前调一次。 */
void registerBuiltinApps();

#endif // STARBOARD_APPS_H
