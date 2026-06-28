#ifndef STARBOARD_APP_H
#define STARBOARD_APP_H

#include <starboard_gui.h>  // menu_item 结构体

// =============================================================================
// starboard_app —— 应用框架(AppBase + AppManager)
//
// 参考 LiClock include/AppManager.h + src/AppManager.cpp,按本项目【纯事件驱动 + 深睡】
// 架构改造为【回合制】(见 docs/DEVELOPMENT.md 阶段3 + 计划文件):
//   - LiClock 是常驻 task_appManager 死循环每 20ms 推进 update() 状态机 + lightsleep/deepsleep。
//   - 本项目每次唤醒 app_main 重跑 → appManager.run() 跑【一回合】:恢复 currentApp →
//     跑 currentApp->setup()(App 内部画帧 / 进 GUI 阻塞交互 / 调 gotoApp·goBack)→
//     循环消费回合内挂起的切换 → 记 lastAppName → deepSleep(不返回)。
//   - App 的 setup() 返回 = 回合结束 = 进深睡;用户下一次按键 = 下一次唤醒 = 下一回合。
//
// 设计要点:
//   - 本组件【只】放框架,不含具体 App(App 放 main/apps/,由 main 显式 registerApp 注册,
//     规避 C++ 跨编译单元静态初始化顺序坑)。
//   - gotoApp/goBack 不再用 LiClock 的 method 延迟状态机,改为【回合内立即链式切换】:
//     设 pendingSwitch/pendingBack,run() 的 do-while 循环消费。
//   - 深睡后 RAM 全丢,current/appStack 不跨深睡;仅靠 RTC_DATA_ATTR lastAppName 恢复
//     「上次活跃的可恢复 App」,栈只重建 home+current 两层(墨水屏 App 浅栈,可接受)。
//   - 系统手势:长按中键 → App 列表(openSelector,复用 GUI::menu)。
//
// 生命周期(全 virtual,统一 LiClock 那套 setup 虚 / 其余函数指针的不一致):
//   - setup():回合入口,画帧 + 交互 + 可调 gotoApp/goBack。返回即回合结束。
//   - onExit():切走 / goBack 时清理。
//   - onDeepsleep():进深睡前把需跨深睡的状态存 RTC_DATA_ATTR/NVS。
// =============================================================================

#include <stack>
#include <stdint.h>

class AppBase
{
public:
    const char *name = nullptr;       // 唯一标识,recover 按 name 查
    const char *title = "App";        // App 列表显示名
    const uint8_t *image = nullptr;   // 32x32 XBM 图标,可空
    bool showInList = true;           // 是否进 App 列表
    bool resumable = true;            // 是否记 lastAppName(OOBE 这种一次性的设 false)

    virtual void setup() {}           // 回合入口
    virtual void onExit() {}          // 切走/goBack 清理
    virtual void onDeepsleep() {}     // 进深睡前存状态
    virtual ~AppBase() = default;
};

class AppManager
{
public:
    // ------------------------- 注册与启动 -------------------------
    /** 注册一个 App(由 main 在 run() 前集中调用)。 */
    void registerApp(AppBase *app);
    /** 注销一个 App(从 appList 移除,不 delete 对象;供 Web IDE 增删 Lua App 后同步)。 */
    void unregisterApp(AppBase *app);
    /** 设默认 App(home)。深睡恢复失败 / 冷启动时进它。 */
    void setHome(AppBase *app) { home = app; }
    /** 启动初始化。M5 起会在此据 OOBE 进度改 home,目前为占位。 */
    void begin();

    // ------------------------- 一回合主循环 -------------------------
    /** 跑一回合:恢复 currentApp → 系统手势 → setup 链 → 记名 → deepSleep(不返回)。 */
    void run();

    // ------------------------- App 切换(回合内链式)-------------------------
    void gotoApp(AppBase *app);   // 切到指定 App(设 pendingSwitch,run 循环消费)
    void gotoApp(const char *name);
    void goBack();                // 出栈返回上层
    /** 请求进入 App 列表(设 pendingSelector,run 循环消费)。供 Lua App 退出用。 */
    void requestSelector() { pendingSelector = true; }
    /** 长按中键触发:GUI::menu 列出 showInList 的 App,选中则 gotoApp。 */
    void openSelector();

    // ------------------------- 定时唤醒 / 深睡 -------------------------
    /** App 在 setup 里设本回合后的定时唤醒兜底秒数(deepSleep 用)。 */
    void setWakeupSec(uint32_t sec) { wakeupSec = sec; }
    /** 调 current->onDeepsleep() + hal.goSleep(wakeupSec)。不返回。 */
    void deepSleep();

    // ------------------------- 查询 -------------------------
    AppBase *currentApp() const { return current; }
    AppBase *homeApp() const { return home; }
    // 是否有待切换的 App(Lua gotoApp 设置,run 循环消费)。web IDE 据此判断是否退出。
    bool hasPendingSwitch() const { return pendingSwitch != nullptr; }
    /**
     * 收集 showInList 的 App 到 items 数组,供设置界面做"默认应用"选择器。
     * @param items 菜单数组(调用方分配,大小 MAX_APPS+1)
     * @param apps  AppBase* 数组(同调用方分配,大小 MAX_APPS)
     * @param max   最大条目数
     * @return 实际填充的 App 数量
     */
    int getAppList(menu_item *items, AppBase **apps, int max);

private:
    static constexpr int MAX_APPS = 16;
    AppBase *appList[MAX_APPS] = {};
    int appCount = 0;
    std::stack<AppBase *> appStack; // 深睡后丢失,只恢复 home+current
    AppBase *current = nullptr;
    AppBase *home = nullptr;
    AppBase *pendingSwitch = nullptr; // gotoApp 挂起
    bool pendingBack = false;         // goBack 挂起
    bool pendingSelector = false;     // requestSelector 挂起(Lua App 退出→进列表)
    uint32_t wakeupSec = 0;
    bool inited = false;

    AppBase *findByName(const char *name);
    /** 合并 LiClock GOTOAPP/GOBACK 的"退出旧→清标志→压/出栈"流程。 */
    void switchToApp(AppBase *app, bool isBack);
};

extern AppManager appManager;

#endif // STARBOARD_APP_H
