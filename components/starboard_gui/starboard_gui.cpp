// starboard_gui 实现 —— 见 include/starboard_gui.h
//
// 移植自 LiClock/src/GUI.cpp,三色屏全刷改造:
//   - 无 push/pop_buffer(GxEPD2_3C 无 swapBuffer),弹窗【不恢复背景】、上层重画;
//   - 每次画面变化用 setFullWindow/firstPage/nextPage 全刷一帧。
//
// ★ busy callback 防丢键(SSD1683 三色屏全刷 ~5s,期间按键会丢,见风险#3):
//   GxEPD2 的 _waitWhileBusy 在等 BUSY 的循环里每轮调 _busy_callback(GxEPD2_EPD.cpp)。
//   initInput() 注册一个回调,在里面读三键 isPressing + 上升沿检测,把"按下"事件存进
//   环形队列。GUI 的按键循环改用 waitKeyEvent() 消费队列——无论刷屏(busy callback 填)
//   还是非刷屏(GUI 自己 poll)时段,按键都进同一队列,刷完即响应,一个不丢。

#include "starboard_gui.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>        // vTaskDelay(busy callback 里让 IDLE 喂狗)
#include <starboard_display.h>   // display, u8g2, COL_*, CN_FONT_MAIN
#include <starboard_hal.h>       // hal, btnl/btnc/btnr, pauseButtons
#include <Fonts/FreeSans9pt7b.h> // msgbox_number/time 数字字体

static const uint8_t *const FONT_TITLE = u8g2_font_wqy12_t_gb2312;

static OneButton &btnOf(int pin)
{
    if (pin == PIN_BUTTONC) return hal.btnc;
    if (pin == PIN_BUTTONR) return hal.btnr;
    return hal.btnl;
}

// ==================== 按键事件缓冲(刷屏期间 busy callback 捕获)====================
namespace
{
    constexpr int KEY_BUF_SIZE = 16;
    int8_t keyBuf[KEY_BUF_SIZE];
    int keyHead = 0, keyTail = 0;
    bool lastL = false, lastC = false, lastR = false; // 上次电平(上升沿检测)

    // 软件消抖:同键两次 push 间隔 < DEBOUNCE_MS 视为抖动,忽略。
    // 必要性:pollKeys 用 OneButton::isPressing()(裸 digitalRead)做上升沿检测,无消抖;
    //         机械按键按下抖动 5~20ms 会产生多个上升沿 → 一次按键 push 多次 → 合并窗口内
    //         selected 多次 ++ → 短菜单回绕(表现为"按右键选中框反而回退")。
    //         50ms 能吃掉抖动脉冲(间隔<20ms),又不影响人手正常连按(间隔>100ms)。
    constexpr unsigned long DEBOUNCE_MS = 50;
    unsigned long lastPushL = 0, lastPushC = 0, lastPushR = 0;

    void pushKey(int pin)
    {
        keyBuf[keyTail] = (int8_t)pin;
        int next = (keyTail + 1) % KEY_BUF_SIZE;
        if (next == keyHead) keyHead = (keyHead + 1) % KEY_BUF_SIZE; // 满则丢最旧
        keyTail = next;
    }
    // 带消抖的 push:同键在 DEBOUNCE_MS 内重复触发则丢弃。unsigned 减法对 millis 回绕安全。
    void pushKeyDebounced(int pin)
    {
        unsigned long now = millis();
        unsigned long *t = nullptr;
        if (pin == PIN_BUTTONL) t = &lastPushL;
        else if (pin == PIN_BUTTONC) t = &lastPushC;
        else if (pin == PIN_BUTTONR) t = &lastPushR;
        else return;
        if (now - *t < DEBOUNCE_MS)
            return; // 消抖窗口内,忽略(抖动多余脉冲)
        *t = now;
        pushKey(pin);
    }
    // 读三键 isPressing + 上升沿(未按→按下)→ push 事件(消抖)。刷屏(busy cb)/非刷屏(GUI poll)都调。
    void pollKeys()
    {
        bool l = hal.btnl.isPressing();
        bool c = hal.btnc.isPressing();
        bool r = hal.btnr.isPressing();
        if (!lastL && l) pushKeyDebounced(PIN_BUTTONL);
        if (!lastC && c) pushKeyDebounced(PIN_BUTTONC);
        if (!lastR && r) pushKeyDebounced(PIN_BUTTONR);
        lastL = l; lastC = c; lastR = r;
    }
    int popKey() // -1 = 空
    {
        if (keyHead == keyTail) return -1;
        int pin = keyBuf[keyHead];
        keyHead = (keyHead + 1) % KEY_BUF_SIZE;
        return pin;
    }
    // 清队列 + 以当前电平为基线(避免一进来就把"已按下"误判为边沿)
    void clearKeys()
    {
        keyHead = keyTail = 0;
        lastL = hal.btnl.isPressing();
        lastC = hal.btnc.isPressing();
        lastR = hal.btnr.isPressing();
    }
    // busy callback:GxEPD2 全刷 _waitWhileBusy 每轮调。两件事:
    //   ① pollKeys 捕获全刷期间的按键(防丢键,见风险#3);
    //   ② vTaskDelay(1) 让 IDLE0 任务有机会跑 → 喂 Task Watchdog。否则全刷 5s 忙等
    //      (_waitWhileBusy 用 __yield 只让给同优先级任务,IDLE 优先级 0 拿不到 CPU)
    //      会触发 IDLE0 watchdog panic。1ms 让出 ×5000 轮 ≈ 5s,够喂饱狗。
    void guiBusyCallback(const void *)
    {
        pollKeys();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Lua 运行期注册的停止检查回调;非空时 waitKeyEvent 每轮先调用一次,返回 true 则中断阻塞。
    // 由 starboard_lua 的 luaSysBeginRun 注册为 luaSysPollStop(检查中键长按/超时 + 置停止标志)。
    bool (*s_stopCheck)() = nullptr;
} // namespace

namespace GUI
{
    // 注册 busy callback(display_init 之后调一次)。之后任何全刷的等 BUSY 期间,
    // 按键自动进缓冲。pauseButtons 期间后台 tick 停,但 isPressing 是实时 digitalRead,不受影响。
    void initInput()
    {
        clearKeys();
        display.epd2.setBusyCallback(guiBusyCallback, nullptr);
    }

    void setStopCheck(bool (*check)()) { s_stopCheck = check; }
    void resetStopCheck() { s_stopCheck = nullptr; }

    // 阻塞等一个按键事件(刷屏期间由 busy callback 填,非刷屏由本函数 poll 填)。
    // 每轮先调一次 s_stopCheck:Lua 运行期若注册了停止回调且回调返回 true,则返回 KEY_STOP
    // 哨兵,让上层 menu/msgbox 识别后提前正常返回(避免 Lua 调 GUI 阻塞时卡死、无法休眠/退出)。
    static int waitKeyEvent()
    {
        for (;;)
        {
            if (s_stopCheck && s_stopCheck()) return KEY_STOP;
            pollKeys();
            int k = popKey();
            if (k >= 0) return k;
            delay(10);
        }
    }
    // 等三键全释放(切场景用,防上次的键带入下一轮)
    static void waitAllReleased()
    {
        while (hal.btnl.isPressing() || hal.btnc.isPressing() || hal.btnr.isPressing())
            delay(10);
    }

    bool waitLongPress(int btn)
    {
        // 进入时键已按下;600ms 内一直按住=长按,中途松开=短按。期间顺带 poll 其它键。
        for (int16_t i = 0; i < 60; ++i)
        {
            if (!btnOf(btn).isPressing()) return false;
            pollKeys();
            delay(10);
        }
        return true;
    }

    int waitKey()
    {
        // 阻塞等任意按键事件(复用 GUI 内部按键队列 + 消抖),返回 1/2/3。
        int pin = waitKeyEvent();
        if (pin == PIN_BUTTONC) return 2;
        if (pin == PIN_BUTTONR) return 3;
        return 1; // PIN_BUTTONL
    }

    int tryGetKey()
    {
        pollKeys();
        int pin = popKey();
        if (pin < 0) return 0;
        if (pin == PIN_BUTTONC) return 2;
        if (pin == PIN_BUTTONR) return 3;
        return 1;
    }

    void pollInputs() { pollKeys(); }

    void autoIndentDraw(const char *str, int max_x, int start_x)
    {
        while (*str)
        {
            if (u8g2.getCursorX() >= max_x || *str == '\n')
                u8g2.setCursor(start_x, u8g2.getCursorY() + 18); // 行高适配 wqy16
            if (*str != '\n') u8g2.print(*str);
            ++str;
        }
    }

    void drawWindowsWithTitle(const char *title, int16_t x, int16_t y, int16_t w, int16_t h)
    {
        display.fillRoundRect(x, y, w, h, 4, COL_BG);     // 填充背景(清空区域)
        display.drawRoundRect(x, y, w, h, 4, COL_NORMAL); // 外框
        display.drawFastHLine(x, y + 15, w, COL_NORMAL);  // 标题栏分隔线
        if (title)
        {
            u8g2.setBackgroundColor(COL_BG);
            u8g2.setForegroundColor(COL_NORMAL);
            u8g2.setFont(FONT_TITLE);
            int16_t tw = u8g2.getUTF8Width(title);
            u8g2.setCursor(x + (w - tw) / 2, y + 12);
            u8g2.print(title);
        }
    }

    void msgbox(const char *title, const char *msg)
    {
        constexpr int16_t w = 280, h = 190;
        const int16_t sx = (SCREEN_WIDTH - w) / 2;
        const int16_t sy = (SCREEN_HEIGHT - h) / 2;
        hal.pauseButtons = true;
        display.setFullWindow();
        display.firstPage();
        do
        {
            drawWindowsWithTitle(title, sx, sy, w, h);
            if (msg)
            {
                u8g2.setFont(CN_FONT_MAIN);
                u8g2.setForegroundColor(COL_NORMAL);
                u8g2.setCursor(sx + 10, sy + 42);
                autoIndentDraw(msg, sx + w - 10, sx + 10);
            }
            u8g2.setFont(FONT_TITLE);
            u8g2.setForegroundColor(COL_NORMAL);
            int16_t bw = u8g2.getUTF8Width("确定");
            display.drawRoundRect(sx + w / 2 - 45, sy + h - 34, 90, 24, 4, COL_NORMAL);
            u8g2.setCursor(sx + w / 2 - bw / 2, sy + h - 16);
            u8g2.print("确定");
        } while (display.nextPage());

        clearKeys();          // 刷窗期间的按键不算(用户还没看清),从干净状态接受确认
        if (waitKeyEvent() != KEY_STOP) // 任意键关闭;KEY_STOP 则不 waitAllReleased(交上层处理松键)
            waitAllReleased();
        hal.pauseButtons = false;
    }

    bool msgbox_yn(const char *title, const char *msg, const char *yes, const char *no)
    {
        constexpr int16_t w = 280, h = 190;
        const int16_t sx = (SCREEN_WIDTH - w) / 2;
        const int16_t sy = (SCREEN_HEIGHT - h) / 2;
        if (!yes) yes = "确定(右)";
        if (!no) no = "取消(左)";
        bool result = false;
        hal.pauseButtons = true;
        display.setFullWindow();
        display.firstPage();
        do
        {
            drawWindowsWithTitle(title, sx, sy, w, h);
            if (msg)
            {
                u8g2.setFont(CN_FONT_MAIN);
                u8g2.setForegroundColor(COL_NORMAL);
                u8g2.setCursor(sx + 10, sy + 42);
                autoIndentDraw(msg, sx + w - 10, sx + 10);
            }
            u8g2.setFont(FONT_TITLE);
            u8g2.setForegroundColor(COL_NORMAL);
            int16_t bw;
            display.drawRoundRect(sx + 20, sy + h - 34, 110, 24, 4, COL_NORMAL); // 左:取消
            bw = u8g2.getUTF8Width(no);
            u8g2.setCursor(sx + 20 + (110 - bw) / 2, sy + h - 16);
            u8g2.print(no);
            display.drawRoundRect(sx + w - 130, sy + h - 34, 110, 24, 4, COL_NORMAL); // 右:确定
            bw = u8g2.getUTF8Width(yes);
            u8g2.setCursor(sx + w - 130 + (110 - bw) / 2, sy + h - 16);
            u8g2.print(yes);
        } while (display.nextPage());

        clearKeys();
        for (;;)
        {
            int k = waitKeyEvent();
            if (k == KEY_STOP) { hal.pauseButtons = false; return false; } // 停止:取消,不 waitAllReleased
            if (k == PIN_BUTTONR) { result = true; break; }
            if (k == PIN_BUTTONL) { result = false; break; }
        }
        waitAllReleased();
        hal.pauseButtons = false;
        return result;
    }

    int menu(const char *title, const menu_item options[], int16_t ico_w, int16_t ico_h)
    {
        constexpr int16_t w = 340, h = 270;
        constexpr int16_t bar_w = 6;
        constexpr int16_t n_items = 10;                     // 一屏项数
        constexpr int16_t title_h = 16;                     // 标题栏高
        constexpr int16_t item_h = (h - title_h) / n_items; // 每项高
        const int16_t sx = (SCREEN_WIDTH - w) / 2;
        const int16_t sy = (SCREEN_HEIGHT - h) / 2;
        const int16_t item_w = w - 10 - bar_w;

        int total = 0;
        bool hasIcon = false;
        while (options[total].title != nullptr)
        {
            if (options[total].icon != nullptr) hasIcon = true;
            ++total;
        }
        if (total == 0) return -1;
        const int16_t track_h = h - title_h;
        const int16_t bar_h = (int16_t)((int)n_items * track_h / total);

        int pageStart = 0, selected = 0, barPos = 0;
        bool updated = true;
        hal.pauseButtons = true;
        clearKeys();
        for (;;)
        {
            if (updated) // 进入菜单先刷一帧显示初始菜单;之后每次合并/长按完刷最终态
            {
                updated = false;
                if (selected < pageStart) pageStart = selected;
                else if (selected >= pageStart + n_items) pageStart = selected - n_items + 1;

                display.setFullWindow();
                display.firstPage();
                do
                {
                    drawWindowsWithTitle(title, sx, sy, w, h);
                    int max_items = min((int)n_items, total);
                    for (int i = 0; i < max_items; ++i)
                    {
                        int16_t iy = sy + title_h + item_h * i;
                        if (options[i + pageStart].icon != nullptr && ico_h <= item_h - 2)
                            display.drawXBitmap(sx + 5, iy + (item_h - ico_h) / 2,
                                                options[i + pageStart].icon, ico_w, ico_h, COL_NORMAL);
                        u8g2.setFont(CN_FONT_MAIN);
                        u8g2.setForegroundColor(COL_NORMAL);
                        u8g2.drawUTF8(sx + 5 + (hasIcon ? ico_w + 2 : 0),
                                      iy + item_h - 4, options[i + pageStart].title);
                        if (selected == i + pageStart)
                            display.drawRoundRect(sx + 3, iy, item_w, item_h - 2, 3, COL_NORMAL);
                    }
                    if (total > n_items)
                    {
                        barPos = selected * (track_h - bar_h) / total;
                        display.fillRoundRect(sx + w - bar_w - 2, sy + title_h + barPos,
                                              bar_w, bar_h, 2, COL_NORMAL);
                    }
                } while (display.nextPage());
            }

            int k = waitKeyEvent();
            if (k == KEY_STOP) { hal.pauseButtons = false; return -1; } // 停止:取消,不 waitAllReleased
            if (k == PIN_BUTTONC)
            {
                if (waitLongPress(PIN_BUTTONC)) { selected = 0; updated = true; } // 长按回首页
                else break;                                                       // 短按确认
            }
            else
            {
                // L/R 移动:开 ~300ms 合并窗口,窗口内连续移动叠加、用户停顿才渲染最终位置。
                // (墨水屏慢刷:按 N 次【只刷一次】最终位置;代价——单次按键也延迟 ~300ms 才刷)
                bool moved = false;
                for (int idle = 0; idle < 30; ) // 30×10ms = 300ms 无新移动事件则结束窗口渲染
                {
                    if (k == PIN_BUTTONL) { if (selected == 0) selected = total; --selected; moved = true; idle = 0; }
                    else if (k == PIN_BUTTONR) { ++selected; if (selected == total) selected = 0; moved = true; idle = 0; }
                    else if (k >= 0) { pushKey(k); break; } // 积压的 C 等非移动事件放回队列,下轮处理
                    pollKeys();
                    k = popKey();
                    if (k < 0) { ++idle; delay(10); } // 无新事件:累计空闲;有则立即进下轮(不 delay)
                }
                if (moved) updated = true;
            }
        }
        waitAllReleased();
        hal.pauseButtons = false;
        return selected;
    }

    int msgbox_number(const char *title, uint16_t digits, int pre_value)
    {
        constexpr int16_t w = 220, h = 120;
        const int16_t sx = (SCREEN_WIDTH - w) / 2;
        const int16_t sy = (SCREEN_HEIGHT - h) / 2;
        const int16_t ix = sx + 12, iy = sy + 38;
        const int16_t iw = w - 24, ih = h - 38 - 18;
        if (digits == 0) return 0;
        --digits;
        if (digits > 8) digits = 8;

        hal.pauseButtons = true;
        clearKeys();
        int cur = pre_value;
        int cd = (int)digits; // 当前位(0=个位)
        int pow = 1;
        for (int i = 0; i < cd; ++i) pow *= 10;
        bool changed = true;
        for (;;)
        {
            int k = waitKeyEvent();
            if (k == KEY_STOP) { hal.pauseButtons = false; return cur; } // 停止:返回当前值,不 waitAllReleased
            if (k == PIN_BUTTONL)
            {
                if (waitLongPress(PIN_BUTTONL)) cd = (cd == (int)digits) ? 0 : cd + 1; // 长按=移位
                else cur -= pow;                                                       // 短按=减
                changed = true;
            }
            else if (k == PIN_BUTTONR)
            {
                if (waitLongPress(PIN_BUTTONR)) cd = (cd == 0) ? (int)digits : cd - 1;
                else cur += pow;
                changed = true;
            }
            else if (k == PIN_BUTTONC)
            {
                if (waitLongPress(PIN_BUTTONC)) { cur = pre_value; changed = true; } // 长按=复位
                else break;                                                           // 短按=确认
            }
            if (changed)
            {
                pow = 1;
                for (int i = 0; i < cd; ++i) pow *= 10;
                changed = false;
                display.setFullWindow();
                display.firstPage();
                do
                {
                    drawWindowsWithTitle(title, sx, sy, w, h);
                    display.drawRoundRect(ix, iy, iw, ih, 3, COL_NORMAL);
                    display.setFont(&FreeSans9pt7b);
                    display.setTextColor(COL_NORMAL);
                    display.setCursor(ix + 8, iy + (ih - 9) / 2 + 12);
                    int n = cur;
                    if (n < 0) { display.print('-'); n = -n; }
                    uint8_t d[9];
                    for (int i = 0; i <= (int)digits; ++i) { d[i] = n % 10; n /= 10; }
                    for (int i = (int)digits; i >= 0; --i)
                    {
                        if (i == cd)
                            display.drawFastHLine(display.getCursorX(), display.getCursorY() + 2, 10, COL_NORMAL);
                        display.print(d[i], DEC);
                    }
                } while (display.nextPage());
            }
        }
        waitAllReleased();
        hal.pauseButtons = false;
        return cur;
    }

    int msgbox_time(const char *title, int pre_value)
    {
        constexpr int16_t w = 220, h = 120;
        const int16_t sx = (SCREEN_WIDTH - w) / 2;
        const int16_t sy = (SCREEN_HEIGHT - h) / 2;
        const int16_t ix = sx + 12, iy = sy + 38;
        const int16_t iw = w - 24, ih = h - 38 - 18;
        const int add[4] = {1, 10, 60, 600}; // 个/十分、个/十时 的分钟步进

        hal.pauseButtons = true;
        clearKeys();
        uint8_t cd = 3;
        int cv = pre_value;
        bool changed = true;
        for (;;)
        {
            int k = waitKeyEvent();
            if (k == KEY_STOP) { hal.pauseButtons = false; return cv; } // 停止:返回当前值,不 waitAllReleased
            if (k == PIN_BUTTONL)
            {
                if (waitLongPress(PIN_BUTTONL)) cd = (cd == 3) ? 0 : cd + 1;
                else { cv -= add[cd]; if (cv < 0) cv = 0; }
                changed = true;
            }
            else if (k == PIN_BUTTONR)
            {
                if (waitLongPress(PIN_BUTTONR)) cd = (cd == 0) ? 3 : cd - 1;
                else { cv += add[cd]; if (cv >= 24 * 60) cv = 24 * 60 - 1; }
                changed = true;
            }
            else if (k == PIN_BUTTONC)
            {
                if (waitLongPress(PIN_BUTTONC)) { cv = pre_value; changed = true; }
                else break;
            }
            if (changed)
            {
                uint8_t tb[4];
                tb[3] = (cv / 60) / 10;
                tb[2] = (cv / 60) % 10;
                tb[1] = (cv % 60) / 10;
                tb[0] = (cv % 60) % 10;
                changed = false;
                display.setFullWindow();
                display.firstPage();
                do
                {
                    drawWindowsWithTitle(title, sx, sy, w, h);
                    display.drawRoundRect(ix, iy, iw, ih, 3, COL_NORMAL);
                    display.setFont(&FreeSans9pt7b);
                    display.setTextColor(COL_NORMAL);
                    display.setCursor(ix + 8, iy + (ih - 9) / 2 + 12);
                    for (int i = 3; i >= 0; --i)
                    {
                        if (i == (int)cd)
                            display.drawFastHLine(display.getCursorX(), display.getCursorY() + 2, 10, COL_NORMAL);
                        display.print(tb[i], DEC);
                        if (i == 2) display.print(':'); // HH:MM 冒号
                    }
                } while (display.nextPage());
            }
        }
        waitAllReleased();
        hal.pauseButtons = false;
        return cv;
    }
    void drawLBM(int16_t x, int16_t y, const char *filename, uint16_t color)
    {
        // 自动补 /littlefs/ 前缀(LittleFS 挂载点)
        const char *path = (filename[0] == '/') ? filename : nullptr;
        char fullPath[128];
        if (!path)
        {
            snprintf(fullPath, sizeof(fullPath), "/littlefs/%s", filename);
            path = fullPath;
        }
        FILE *fp = fopen(path, "rb");
        if (!fp)
        {
            Serial.printf("LBM file not found: %s\n", path);
            return;
        }
        uint16_t w, h;
        fread(&w, 2, 1, fp);
        fread(&h, 2, 1, fp);
        size_t imgsize;
        uint16_t tmp = w / 8;
        if (w % 8 != 0)
            tmp++;
        imgsize = tmp * h;
        uint8_t *img = (uint8_t *)malloc(imgsize);
        if (!img)
        {
            Serial.printf("LBM malloc failed\n");
            fclose(fp);
            return;
        }
        fread(img, 1, imgsize, fp);
        fclose(fp);
        display.drawXBitmap(x, y, img, w, h, color);
        free(img);
    }
} // namespace GUI
