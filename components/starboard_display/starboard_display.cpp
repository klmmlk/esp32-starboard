// starboard_display 实现 —— 见 include/starboard_display.h
//
// 阶段2a: display 全局实例(三色屏 GxEPD2_3C)+ u8g2 中文 + 统一全刷初始化。
// display 实例从原 main.cpp 抽出,放此组件唯一一处定义,供 main/gui/app 共用。

#include "starboard_display.h"

// ---- 实例定义(唯一一处)----
// 引脚全部从 starboard_config 取,改引脚只改那个头文件。
StarboardDisplay display(
    GxEPD2_420c_GDEY042Z98(
        /*CS=*/   CONFIG_SPI_CS,
        /*DC=*/    CONFIG_PIN_DC,
        /*RST=*/   CONFIG_PIN_RST,
        /*BUSY=*/  CONFIG_PIN_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2;

void display_init()
{
    display.init(115200);   // 参数=诊断日志波特率(顺带初始化 Serial)
    // setRotation 由 appManager.run() 从 NVS 读,screen_orient 设置才生效;这里不设默认
    display.setTextColor(COL_NORMAL);
    display.fillScreen(COL_BG);

    // u8g2 绑定到 display(传引用)。此时 display 全局对象已构造完成,无静态初始化顺序问题。
    u8g2.begin(display);
    u8g2.setFont(CN_FONT_MAIN);
    u8g2.setForegroundColor(COL_NORMAL);
    u8g2.setBackgroundColor(COL_BG);
}

void display_deinit()
{
    display.hibernate();   // 刷完进深睡省电(沿用原 main.cpp 行为)
}
