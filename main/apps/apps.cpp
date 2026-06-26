// 内置 App 注册聚合。新增 App:在其 appXxx.cpp 暴露 `AppBase* const appXxx`,
// 在此 registerApp(appXxx)。home 默认主时钟(M5 起 begin() 会据 OOBE 进度改 home)。

#include "apps.h"
#include <starboard_app.h>

void registerBuiltinApps()
{
    appManager.registerApp(appClock);
    appManager.registerApp(appSettings);
    appManager.registerApp(appOOBE); // showInList=false,不进列表,但需注册供 begin() findByName
    appManager.registerApp(appOTA);
    appManager.registerApp(appWebIDE);
    appManager.setHome(appClock);    // 默认 home = 主时钟(begin 据 oobe 进度可能改写为 OOBE)
}
