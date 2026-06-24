// ESP-IDF 版墨水屏测试程序
//
// 当前:【纯事件驱动 · 全彩全刷】验证(阶段2b 前置)。
//   背景:三色屏(GDEY042Z98)黑白局刷实测【不可用】(串色/残影/仍全屏闪,见
//         docs/DEVELOPMENT.md 风险#3),Waveshare 官方 4in2b_V2 库亦无 partial API。
//         故本项目锁定【全彩全刷】;刷新策略经与用户确认采用【纯事件驱动】——
//         平时深睡,仅"按键唤醒 / 天气更新 / 闹钟到点"时全刷一帧,屏幕静态保持。
//
//   本 demo 验证:① 纯事件驱动刷新闭环 ② 【实测全刷耗时】(后续所有交互的时间基准)。
//   流程:app_main → hal.init(WiFi+NTP) → display_init → 全刷一帧(测耗时)
//        → 串口打印耗时 → 进深睡(纯按键唤醒) → 按键 → app_main 重跑(循环)。
//
//   ⚠️ 注意:hal.init 每次唤醒会重连 WiFi+NTP(几秒)。实测的【刷新耗时】只计
//      firstPage~nextPage 那段(见 refreshMainFrame 内 millis),不含 WiFi 延迟。
//      阶段3 起会把"唤醒后默认不连 WiFi、仅在要刷新天气时连"下沉到 App 逻辑。

#include <Arduino.h>
#include <starboard_config.h>
#include <starboard_hal.h>
#include <starboard_display.h>

// 全刷一帧(主时钟骨架)+ 返回刷新耗时(ms)。
// 绘制须在 do{}while(nextPage) 循环内【每页重画】(GxEPD2 分页全刷规则)。
static uint32_t refreshMainFrame()
{
    hal.getTime(); // 填 hal.timeinfo / hal.now(深睡期间 RTC 维持走时,唤醒后时间仍准)

    display.setFullWindow();
    display.firstPage();
    uint32_t t0 = millis(); // 只测刷新段(WiFi 延迟不计)
    do
    {
        display.fillScreen(COL_BG);

        // 顶部:大号时间。时间有效性看年份(深睡期间 RTC 维持走时,
        // 即便本次唤醒没连上 NTP/WiFi,只要历史上对过时就有准时间),不再只依赖 timeSynced。
        display.setTextColor(COL_NORMAL);
        display.setTextSize(7);
        display.setCursor(20, 20);
        char tbuf[8];
        const bool timeValid = hal.timeinfo.tm_year > 120; // tm_year 从 1900 起,>120 即 2020 年后
        snprintf(tbuf, sizeof(tbuf), timeValid ? "%02d:%02d" : "--:--",
                 hal.timeinfo.tm_hour, hal.timeinfo.tm_min);
        display.print(tbuf);

        // 中部:中文日期 + 星期(u8g2)
        static const char *const wk[] = {"日", "一", "二", "三", "四", "五", "六"};
        char dbuf[40];
        snprintf(dbuf, sizeof(dbuf), "%d月%d日 星期%s",
                 hal.timeinfo.tm_mon + 1, hal.timeinfo.tm_mday, wk[hal.timeinfo.tm_wday]);
        u8g2.setForegroundColor(COL_NORMAL);
        u8g2.setFont(CN_FONT_MAIN);
        u8g2.drawUTF8(20, 175, dbuf);

        // 中部:红色强调行(验证红色全刷渲染)
        u8g2.setForegroundColor(COL_ALERT);
        u8g2.drawUTF8(20, 220, "事件驱动 · 全刷验证");

        // 底部:操作提示(黑)
        u8g2.setForegroundColor(COL_NORMAL);
        u8g2.drawUTF8(20, 285, "按任意键刷新一帧 · 平时深睡");
    } while (display.nextPage());
    return millis() - t0;
}

// 【刷新策略】事件 + 定时兜底(混合)。
//   - 事件:按键唤醒 → 刷新一帧(用户主动看)。
//   - 定时:每 REFRESH_INTERVAL_SEC 秒兜底唤醒刷新一次(挂着不动时信息不过时太久)。
//   底层:hal.goSleep(sec) 同时开 timer(sec 后)+按键(ext1)两路唤醒,先到先触发。
//   阶段3 AppManager 会把它封装成 setTimer()/nextWakeup(移植自 LiClock)供各 App 调用。
static constexpr uint32_t REFRESH_INTERVAL_SEC = 5 * 60; // 默认 15 分钟兜底刷新

extern "C" void app_main()
{
    initArduino();
    hal.init();        // 含 WiFi+NTP(首次对时;唤醒后会重连,几秒;连不上 8s 超时放弃)
    display_init();

    const char *why = !hal.wakeUpFromDeepSleep ? "首次上电"
                    : hal.wakeupButton >= 0    ? "按键唤醒"
                    :                            "定时唤醒";
    Serial.printf("[DEMO] 唤醒类型:%s\n", why);

    // SmartConfig 配网中:不能睡!否则用户来不及用微信推 WiFi。
    // 显示配网提示,保持唤醒轮询,直到连上(Connected)。
    if (hal.wifiState == HAL::WifiState::Provisioning)
    {
        Serial.println("[DEMO] 配网模式:保持唤醒等待 SmartConfig,不进深睡。");
        display.setFullWindow();
        display.firstPage();
        do
        {
            display.fillScreen(COL_BG);
            u8g2.setForegroundColor(COL_NORMAL);
            u8g2.setFont(CN_FONT_MAIN);
            u8g2.drawUTF8(20, 100, "正在配网...");
            u8g2.drawUTF8(20, 150, "请用微信「乐鑫 AirKiss」");
            u8g2.drawUTF8(20, 185, "或 ESPTouch 推送 WiFi");
            u8g2.setForegroundColor(COL_ALERT);
            u8g2.drawUTF8(20, 250, "仅支持 2.4G · 请勿休眠");
        } while (display.nextPage());

        // 配网完成 = wifiState 变 Connected(IP_EVENT_GOT_IP)。最长等 5 分钟,超时也认(下次重试)。
        for (uint32_t i = 0; i < 5 * 60 && hal.wifiState != HAL::WifiState::Connected; ++i)
        {
            delay(1000);
        }
        Serial.println("[DEMO] 配网等待结束,刷新主界面...");
    }

    uint32_t ms = refreshMainFrame();
    Serial.printf("[DEMO] 全刷耗时 %lu ms (%.1f s)\n", (unsigned long)ms, ms / 1000.0f);
    Serial.printf("[DEMO] 刷完 → 进深睡。按键 或 %lu 分钟后 自动唤醒刷新。\n",
                  (unsigned long)(REFRESH_INTERVAL_SEC / 60));

    hal.goSleep(REFRESH_INTERVAL_SEC); // 事件(按键)+定时(兜底)双通道,先到先唤醒
}
