#ifndef STARBOARD_DISPLAY_H
#define STARBOARD_DISPLAY_H

// =============================================================================
// starboard_display —— 三色墨水屏 + 中文字体 封装层
//
// 设计原则: 刻意【不】依赖 starboard_hal/gui,显示层独立,可单独编译。
//           提供 display 全局实例、语义颜色、中文字体(u8g2)、统一的全刷初始化。
//
// 三色屏策略(见 docs/DEVELOPMENT.md 阶段2):
//   - 4.2" GDEY042Z98(400×300 红/黑/白)底层走【全屏刷新】(hasFastPartialUpdate=false),
//     本项目统一用 setFullWindow/firstPage/nextPage 全刷,【不碰 partial】,避免串色残影。
//   - 语义颜色是【本项目新增】(LiClock 是黑白屏无红色);红色约定:低电量/充电/闹钟/
//     错误/天气预警=红,其余黑。GUI/App 层只用 COL_* 宏,不要直接用 GxEPD_RED。
//
// 参考: GxEPD2 GxEPD2_GFX_Example(分页全刷官方写法)、U8g2_for_Adafruit_GFX。
// =============================================================================

#include <GxEPD2_3C.h>                    // GxEPD2_3C 模板 + GxEPD_* 颜色(GxEPD2_3C.h 内含 GxEPD2.h)
#include <U8g2_for_Adafruit_GFX.h>        // U8G2_FOR_ADAFRUIT_GFX + 字体符号声明
#include <starboard_config.h>             // CONFIG_SPI_CS / DC / RST / BUSY(顶层 #define)

// ---- 三色屏语义颜色(本项目新增约定;LiClock 黑白屏无此概念)----
#define COL_NORMAL  GxEPD_BLACK   // 0x0000  正文/常规
#define COL_ALERT   GxEPD_RED     // 0xF800  强调/告警(低电量/充电/闹钟/错误/天气预警)
#define COL_BG      GxEPD_WHITE   // 0xFFFF  背景

// ---- 显示类型别名(隐藏模板样板;以后换屏只改这一行)----
// page_height 取整屏高度(=HEIGHT),需能放下整屏 black+color 双缓冲,已验证内存够。
using StarboardDisplay =
    GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT>;

// ---- 全局实例(main/gui/app 共用同一个)----
extern StarboardDisplay display;
extern U8G2_FOR_ADAFRUIT_GFX u8g2;

// ---- 生命周期 ----
/**
 * @brief 初始化显示。应在 hal.init() 之后、绘制之前调用。
 *        内部: display.init(115200) + 默认 rotation/textColor + u8g2.begin(display)
 *               + setFont(CN_FONT_MAIN)。
 */
void display_init();
/** 刷完进入深睡省电(对应 GxEPD2 的 hibernate);之后再刷新需重新 display_init()。 */
void display_deinit();

// ---- 屏幕空闲休眠(电子纸 bistable:关驱动 IC 不改显示内容,纯省电,下次刷新自动恢复)----
// 由 busy callback 打戳 + 各长驻循环协作触发,详见 starboard_display.cpp。
void display_notifyRefresh();                           // 刷新打戳(busy callback 内调,勿直接用)
bool display_idleHibernate(unsigned long idleSec = 10); // 距上次刷新>idleSec 则 hibernate;返回是否刚进入休眠

// ---- 中文字体(默认主字体)----
// wqy16_t_gb2312 是本库里【唯一同时含 ASCII + 全 GB2312 汉字】的字体(318KB)。
// 其余含中文的变体要么不含 ASCII(unifont_t_chinese1/2/3 要分3段)、要么是日文
// (b10/b12/b16_t_japanese*)、要么是纯拉丁(crox*/unifont_tf 仅几KB无汉字)。
// wqy 是细体点阵(t=等宽),库内无更粗的中文替代;嫌细见 displayDemo 注释的加粗方案。
#define CN_FONT_MAIN  u8g2_font_wqy16_t_gb2312   // 16px GB2312 全字库

#endif // STARBOARD_DISPLAY_H
