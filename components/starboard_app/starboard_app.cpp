// starboard_app 实现 —— 见 include/starboard_app.h
//
// 回合制深睡驱动(对比 LiClock 的常驻 update() 死循环,见头文件注释)。
// 关键:current/appStack 是 AppManager(全局对象)成员,深睡=断电式重启后随 RAM 丢失,
//       每次唤醒 app_main 重跑 → appManager 全局对象重新构造 → current=nullptr →
//       run() 走恢复分支,靠 RTC_DATA_ATTR lastAppName 找回上次活跃的可恢复 App。

#include "starboard_app.h"
#include <Arduino.h>
#include <string.h>
#include <starboard_config.h> // PIN_BUTTONC
#include <starboard_hal.h>    // hal.wakeUpFromDeepSleep / wakeupButton / goSleep / pauseButtons
#include <starboard_gui.h>    // GUI::waitLongPress / GUI::menu / menu_item
#include <starboard_display.h> // display.hibernate(保持期 10 秒后休眠屏幕)

// 跨深睡:上次活跃的【可恢复】App 名。冷启动靠 hal.wakeUpFromDeepSleep 门控
// (冷启动=false → 不读它,直接走 home),避免读到未初始化的 RTC 残留垃圾值。
RTC_DATA_ATTR static char lastAppName[32] = {0};

void AppManager::registerApp(AppBase *app)
{
    if (!app || appCount >= MAX_APPS)
        return;
    appList[appCount++] = app;
}

AppBase *AppManager::findByName(const char *name)
{
    if (!name)
        return nullptr;
    for (int i = 0; i < appCount; ++i)
        if (strcmp(appList[i]->name, name) == 0)
            return appList[i];
    return nullptr;
}

void AppManager::begin()
{
    inited = true;
    // 未完成 OOBE 引导(pref "oobe" < 3)→ home = 引导 App,开机即进引导;
    // 否则保持 registerBuiltinApps 设的 home(主时钟)。
    if (hal.pref.getInt("oobe", 0) < 3)
    {
        AppBase *oobe = findByName("oobe");
        if (oobe)
            home = oobe;
    }
}

void AppManager::gotoApp(AppBase *app)
{
    if (!app)
        return;
    pendingSwitch = app;
    pendingBack = false;
}

void AppManager::gotoApp(const char *name)
{
    gotoApp(findByName(name));
}

void AppManager::goBack()
{
    if (appStack.empty())
        return;
    pendingBack = true;
    pendingSwitch = nullptr;
}

// 合并 LiClock AppManager.cpp 里 GOTOAPP(403-436) / GOBACK(437-470) 两段重复的切换流程。
void AppManager::switchToApp(AppBase *app, bool isBack)
{
    if (!app)
        return;
    if (current && current != app)
        current->onExit(); // 退出旧 App
    if (isBack)
    {
        if (!appStack.empty())
            appStack.pop();
    }
    else // gotoApp:把旧 current 压栈作 goBack 退路
    {
        if (current && current != app)
            appStack.push(current);
    }
    current = app;
    wakeupSec = 0; // 定时器是当前 App 私有,切换时清零(LiClock 同款)
}

void AppManager::openSelector()
{
    // 收集 showInList 的 App 填进菜单;选中则 gotoApp。
    menu_item items[MAX_APPS + 1];
    AppBase *apps[MAX_APPS];
    int n = 0;
    for (int i = 0; i < appCount; ++i)
    {
        if (appList[i]->showInList)
        {
            apps[n] = appList[i];
            items[n].icon = appList[i]->image;
            items[n].title = appList[i]->title;
            ++n;
        }
    }
    items[n].icon = nullptr;
    items[n].title = nullptr; // 结束哨兵
    if (n == 0)
        return;
    int sel = GUI::menu("应用列表", items);
    if (sel >= 0 && sel < n)
        gotoApp(apps[sel]);
    // menu 无"取消"(中键短按即确认),总会返回一个索引;sel<0 仅在 options 为空时。
}

void AppManager::run()
{
    if (!inited)
        begin();

    // 1. 恢复 current(每次唤醒 current 都为 nullptr:全局对象随深睡 RAM 丢失后重新构造)
    if (!current)
    {
        while (!appStack.empty())
            appStack.pop();
        AppBase *resume = (hal.wakeUpFromDeepSleep && lastAppName[0]) ? findByName(lastAppName) : nullptr;
        current = resume ? resume : home;
        if (home && current != home)
            appStack.push(home); // goBack 退路:深睡后只重建 home+current 两层
        Serial.printf("[APP] 恢复 App=%s (唤醒=%s, 键=%d)\n",
                      current ? current->name : "(null)",
                      hal.wakeUpFromDeepSleep ? "深睡" : "冷启动", hal.wakeupButton);
    }

    // 2. 系统手势:中键唤醒 → 长按(>=500ms)→ App 列表;松手(短按)→ 跑当前 App
    //    不用 GUI::waitLongPress:后者依赖 pollKeys 的上升沿(edge: 未按→按下),
    //    但唤醒时键已按着,pollKeys 首次执行会产生「伪上升沿」被 if(polled&&btnIsPress)
    //    吞掉 → 永远进不了长按计时 → 空循环直到松手 → 误判为短按。
    //    改用 digitalRead + 持续计时,不依赖上升沿,从唤醒时刻起算。
    if (hal.wakeUpFromDeepSleep && hal.wakeupButton == PIN_BUTTONC)
    {
        hal.pauseButtons = true; // 检测期间停后台 tick(isPressing 不受影响但防 HAL 回调干扰)
        unsigned long pressStart = millis();
        bool longPress = false;
        for (;;)
        {
            if (digitalRead(PIN_BUTTONC) == LOW)
            {
                if (millis() - pressStart >= 500) { longPress = true; break; }
                delay(10);
            }
            else break; // 松手 → 短按
        }
        hal.pauseButtons = false;
        if (longPress)
        {
            // 等键完全松再弹列表(否则 GUI::menu 入口 keyBuf 空,waitKeyEvent 等不到事件)
            while (digitalRead(PIN_BUTTONC) == LOW) delay(10);
            delay(50); // 防抖
            openSelector(); // 内部 GUI::menu 阻塞;选中则设 pendingSwitch
        }
    }

    step3: // 保持期按键重画后跳回:重新跑 current App setup 或处理 pending 切换
    // 3. 跑当前 App,循环消费回合内挂起的 gotoApp/goBack(链式切换)
    //    一个回合内可能经历:clock →(长按中)selector → settings →(改完)goBack → clock,
    //    全靠 pending 标志在 do-while 里逐步推进,直到无挂起。
    do
    {
        if (pendingSwitch)
        {
            switchToApp(pendingSwitch, false);
            pendingSwitch = nullptr;
        }
        else if (pendingBack)
        {
            pendingBack = false;
            if (!appStack.empty())
                switchToApp(appStack.top(), true);
        }
        if (current)
            current->setup();
    } while (pendingSwitch || pendingBack);

    // 4. 记 lastAppName(可恢复 App 才记,跨深睡);不可恢复的清空(下次走 home)
    if (current && current->resumable && current->name)
    {
        strncpy(lastAppName, current->name, sizeof(lastAppName) - 1);
        lastAppName[sizeof(lastAppName) - 1] = '\0';
    }
    else
    {
        lastAppName[0] = '\0';
    }

    // 5. 保持期:画帧后保持唤醒 N 秒,按键则重画/列表;无操作超时后深睡
    //    - 保持期时长来自 hal.pref(\"sleep_to\",默认 60 秒,最小 10 秒)
    //    - 前 10 秒屏幕正常显示;10 秒后 display.hibernate()(屏驱动关电源,内容保留显示不耗电)
    //    - 期间任意键:若中键长按→列表;若短按/其他键→重画当前 App(setup 自动 powerUp 唤醒屏幕)
    //    - 重画后重置超时重新保持
    {
        unsigned long staySec = (unsigned long)hal.pref.getInt("sleep_to", 60);
        if (staySec < 10) staySec = 10;
        unsigned long stayMax = staySec * 1000UL;
        unsigned long stayStart = millis();
        bool hibernateDone = false;

        while (millis() - stayStart < stayMax)
        {
            bool l = digitalRead(PIN_BUTTONL) == LOW;
            bool c = digitalRead(PIN_BUTTONC) == LOW;
            bool r = digitalRead(PIN_BUTTONR) == LOW;

            if (l || c || r)
            {
                hal.pauseButtons = true;

                // 中键:长按(>=500ms)→ App 列表,短按→重画
                if (c)
                {
                    unsigned long pStart = millis();
                    bool lp = false;
                    while (digitalRead(PIN_BUTTONC) == LOW)
                    {
                        if (millis() - pStart >= 500) { lp = true; break; }
                        delay(10);
                    }
                    if (lp)
                    {
                        while (digitalRead(PIN_BUTTONC) == LOW) delay(10);
                        delay(50);
                        hal.pauseButtons = false;
                        openSelector();
                        // 若选中了 App(设了 pendingSwitch),跳回 step3 处理链式切换
                        if (pendingSwitch || pendingBack) goto step3;
                        // 取消→不切换,继续保持期(重画一帧)
                        if (current) current->setup();
                        stayStart = millis(); hibernateDone = false;
                        continue;
                    }
                    // short press fallthrough → 重画
                }

                // 短按(左/中/右键):等全松,重画当前 App
                while (digitalRead(PIN_BUTTONL) == LOW || digitalRead(PIN_BUTTONC) == LOW || digitalRead(PIN_BUTTONR) == LOW)
                    delay(10);
                delay(50);
                hal.pauseButtons = false;
                if (current) current->setup();
                stayStart = millis(); hibernateDone = false;
                continue;
            }

            // 无按键:10 秒后 hibernate 屏幕(关驱动电源,内容保持)
            if (!hibernateDone && (millis() - stayStart >= 10000))
            {
                display.hibernate();
                hibernateDone = true;
            }
            delay(50);
        }
    }

    // 6. 深睡(不返回):app_main 到此结束
    deepSleep();
}

void AppManager::deepSleep()
{
    if (current)
        current->onDeepsleep();
    Serial.printf("[APP] 进入深睡:下次=%lus 后或按键唤醒,当前 App=%s\n",
                  (unsigned long)wakeupSec, current ? current->name : "(null)");
    hal.goSleep(wakeupSec); // timer(兜底)+ 按键(ext1)双路,先到先唤醒,不返回
}

AppManager appManager;
