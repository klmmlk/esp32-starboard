// ESP-IDF 版墨水屏主程序 —— 阶段3:AppManager 应用框架(回合制深睡驱动)
//
//   app_main → hal.init(串口/时区/NVS/按键/唤醒原因;不含 WiFi)→ display_init
//            → GUI::initInput(注册 busy callback:全刷 ~5s 期间按键进缓冲不丢)
//            → registerBuiltinApps(注册内置 App + 设 home)→ appManager.begin
//            → appManager.run(一回合:恢复 App → 系统手势 → setup 链 → 深睡,不返回)
//
//   核心模型:一次唤醒 = app_main 重跑 = appManager.run() 一回合。App 的 setup() 返回
//   即回合结束、进深睡;用户下一次按键 = 下一次唤醒 = 下一回合。详见 docs/DEVELOPMENT.md 阶段3。

#include <Arduino.h>
#include <starboard_config.h>
#include <starboard_hal.h>
#include <starboard_display.h>
#include <starboard_gui.h>
#include <starboard_app.h>
#include <starboard_audio.h>
#include "apps/apps.h"
#include <lua_app_wrapper.h>

extern "C" void app_main()
{
    initArduino();
    hal.init();          // 不含 WiFi(阶段3 起 WiFi 按需:OOBE 配网 / 天气 App 才调 hal.wifiInit)
    display_init();
    audio.init();        // MAX98357A I2S(须在 display_init 后:接管 GPIO13,屏幕 MISO 未接无影响)
    GUI::initInput();    // busy callback 防丢键:全刷期间按键进缓冲,GUI/App 消费;且周期喂狗(见实现)

    registerBuiltinApps(); // 注册内置 App + 设 home
    scanAndRegisterLuaApps(); // 扫描 /littlefs/apps/,注册 Lua App
    appManager.begin();    // (M5 起会据 OOBE 进度改 home)
    appManager.run();      // 一回合,末尾 goSleep 不返回
}
