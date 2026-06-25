// appClock —— 主时钟(默认 home App)
//
// 阶段3 最小版:仅本地 RTC 时间(去天气,天气 API 后置)。画面搬自原 main.cpp 的
// refreshMainFrame()。回合制语义:setup() 画一帧即返回 → appManager 深睡;
// 「短按键刷新」靠【下次唤醒重跑 setup】天然实现,App 内不需按键循环。

#include <starboard_app.h>
#include <starboard_hal.h>     // hal.getTime / timeinfo
#include <starboard_display.h> // display / u8g2 / COL_* / CN_FONT_MAIN
#include <Arduino.h>

namespace
{
class AppClock : public AppBase
{
public:
    AppClock()
    {
        name = "clock";
        title = "时钟";
        resumable = true;
        showInList = true;
    }

    void setup() override
    {
        hal.getTime(); // 填 hal.timeinfo / hal.now(深睡期间 RTC 维持走时,唤醒后时间仍准)

        display.setFullWindow();
        display.firstPage();
        do
        {
            display.fillScreen(COL_BG);

            // 顶部:大号时间。时间有效性看年份(深睡 RTC 维持走时,历史上对过时即准)。
            display.setTextColor(COL_NORMAL);
            display.setTextSize(7);
            display.setCursor(20, 20);
            char tbuf[8];
            const bool timeValid = hal.timeinfo.tm_year > 120; // tm_year 从 1900 起,>120 即 2020 年后
            snprintf(tbuf, sizeof(tbuf), timeValid ? "%02d:%02d" : "--:--",
                     hal.timeinfo.tm_hour, hal.timeinfo.tm_min);
            display.print(tbuf);

            // 中部:中文日期 + 星期
            static const char *const wk[] = {"日", "一", "二", "三", "四", "五", "六"};
            char dbuf[40];
            snprintf(dbuf, sizeof(dbuf), "%d月%d日 星期%s",
                     hal.timeinfo.tm_mon + 1, hal.timeinfo.tm_mday, wk[hal.timeinfo.tm_wday]);
            u8g2.setForegroundColor(COL_NORMAL);
            u8g2.setFont(CN_FONT_MAIN);
            u8g2.drawUTF8(20, 175, dbuf);

            // 中部:红色强调行(验证三色屏红色渲染)
            u8g2.setForegroundColor(COL_ALERT);
            u8g2.drawUTF8(20, 220, "事件驱动 · 全刷验证");

            // 底部:操作提示
            u8g2.setForegroundColor(COL_NORMAL);
            u8g2.drawUTF8(20, 285, "长按中键:应用列表");
        } while (display.nextPage());

        appManager.setWakeupSec(5 * 60); // 5 分钟兜底刷新(挂着不动时信息也不至于过时太久)
    }
};
AppClock appClockInst;
} // namespace

// 以基类指针暴露给 main/apps/apps.cpp 注册(具体子类 AppClock 留在本 TU 内部链接)。
// ⚠️ 必须 extern:C++ 里 namespace 作用域的 const 变量【默认内部链接】,不加 extern 的话
//    apps.cpp 的 extern 声明链接不到 → undefined reference。extern 强制外部链接(保留 const)。
extern AppBase *const appClock = &appClockInst;
