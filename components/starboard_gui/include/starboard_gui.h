#ifndef STARBOARD_GUI_H
#define STARBOARD_GUI_H

// =============================================================================
// starboard_gui —— 通用 GUI(消息框 / 菜单 / 数字·时间输入)
//
// 移植自 LiClock/src/GUI.cpp,适配三色屏全刷架构。核心改造(见
// docs/DEVELOPMENT.md 阶段2 + 计划文件):
//   1. 删 LiClock 的 push_buffer/pop_buffer(本项目 GxEPD2_3C 无 swapBuffer/
//      copyBuffer/current_buffer_idx)。弹窗【不恢复背景】,返回后由上层 App
//      整屏重画——阶段3 AppManager 须遵守此约定。
//   2. 每次画面更新用全刷分页 setFullWindow/firstPage/do{画}while(nextPage)。
//   3. 颜色硬编码 0/1 → 语义色 COL_NORMAL/COL_BG/COL_ALERT。
//   4. 坐标 296×128 → 400×300(SCREEN_WIDTH/HEIGHT 居中,窗口放大)。
//   5. 按键用 hal.btnl/btnc/btnr.isPressing() 阻塞轮询(OneButton 已 public);
//      GUI 期间置 hal.pauseButtons=true 暂停后台 tick,防左键长按触发深睡。
//
// 未移植(依赖未就绪):drawLBM/fileDialog(需 LittleFS)、graph.cpp(天气专用)。
// =============================================================================

#include <stdint.h>
#include <starboard_config.h> // SCREEN_WIDTH/HEIGHT(纯宏头,无依赖)

// 菜单项:可选 XBM 图标 + 标题。数组末项须用 {nullptr, nullptr} 作结束哨兵。
typedef struct
{
    const uint8_t *icon; // XBM 位图,可为 NULL
    const char *title;
} menu_item;

namespace GUI
{
    // 注册 busy callback(在 display_init 之后、首次 GUI 交互前调一次)。
    // 之后全刷等 BUSY 期间(~5s)按键自动进缓冲,GUI 消费——刷屏期间按键不丢。
    void initInput();

    // ---- Lua 运行期停止检查(让 menu/msgbox 等阻塞 GUI 能被「中键长按>1s / 无操作超时」中断)----
    // Lua 进入运行时注册一个回调;之后所有阻塞 GUI 内部的 waitKeyEvent 每轮先调一次回调,
    // 回调返回 true(请求停止)则 waitKeyEvent 返回哨兵 KEY_STOP,各阻塞 GUI 识别后
    // 【提前正常返回】(跳过 waitAllReleased、复位 pauseButtons),由 Lua 绑定层据
    // luaStopRequested() luaL_error。回调内只做轻量检查 + 置停止标志,禁止重操作/longjmp
    // (GUI 层无 lua_State,且要保证 C++ 栈正常展开,避免 menu 里 new 的内存泄漏)。
    static constexpr int KEY_STOP = -2;      // waitKeyEvent 的停止哨兵(区别于 -1=队列空)
    void setStopCheck(bool (*check)());      // 注册回调(nullptr=清除)
    void resetStopCheck();

    // 检测长按:进入时键已按下,持续按住 ~600ms 返回 true(长按),中途松开返回 false(短按)。
    // btn 取 PIN_BUTTONL / PIN_BUTTONC / PIN_BUTTONR。
    bool waitLongPress(int btn);

    // 阻塞等待任意一个按键被按下,返回按键编号:1=左 2=中 3=右(用于 Lua 事件循环)。
    // 内部复用按键事件队列(pollKeys + 消抖),无需先 initInput 也可工作。
    int waitKey();

    // 非阻塞取一个按键事件:有则返回 1=左 2=中 3=右,无事件返回 0。
    // 供 Lua 层在轮询循环里配合停止标志使用(避免 waitKey 死等导致无法强停)。
    int tryGetKey();

    // 主动轮询按键(上升沿→事件队列),不返回。供主线程长 delay 期间调用避免丢键。
    void pollInputs();

    // 自动换行绘制(游标式:依赖 u8g2 游标,调用前先 setCursor + setFont)。
    // 行高按 wqy16(16px)硬编码为 18;换字体需同步改。
    void autoIndentDraw(const char *str, int max_x, int start_x = 2);

    // 画带标题栏的圆角窗口(语义色)。默认铺满整屏;标题栏用 wqy12 小字居中。
    void drawWindowsWithTitle(const char *title = nullptr,
                              int16_t x = 0, int16_t y = 0,
                              int16_t w = SCREEN_WIDTH, int16_t h = SCREEN_HEIGHT);

    // 标准消息框:画窗+内容+"确定"按钮,全刷一帧,任意键关闭。返回后上层重画背景。
    void msgbox(const char *title, const char *msg);

    // 是/否对话框:右键=是(返回 true),左键=否(返回 false)。
    bool msgbox_yn(const char *title, const char *msg,
                   const char *yes = nullptr, const char *no = nullptr);

    // 数字输入:digits=位数(1/2…)。短按左/右=减/加当前位,长按左/右=移位,
    //           长按中=复位,短按中=确认返回。
    int msgbox_number(const char *title, uint16_t digits, int pre_value);

    // 时间输入(HH:MM):返回当日分钟数 0..1439。按键同 msgbox_number。
    int msgbox_time(const char *title, int pre_value);

    // 菜单:左右键移动选中(【移动即全刷】,选中框实时跟随),中键确认返回索引,
    //       长按中键回到首项。options 末项须 {nullptr,nullptr}。
    int menu(const char *title, const menu_item options[],
             int16_t ico_w = 8, int16_t ico_h = 8);

    // 绘制 LBM 格式单色位图(xbm)文件(前景色由 color 指定,背景透明)。
    // filename 若非绝对路径,自动补 /littlefs/ 前缀(LittleFS 挂载点)。
    void drawLBM(int16_t x, int16_t y, const char *filename, uint16_t color);
} // namespace GUI

#endif // STARBOARD_GUI_H
