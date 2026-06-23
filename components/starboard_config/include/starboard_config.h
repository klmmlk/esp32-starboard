#ifndef STARBOARD_CONFIG_H
#define STARBOARD_CONFIG_H

// =============================================================================
// starboard_config —— 全局引脚定义、屏幕尺寸、常量
//
// ⚠️ 重要: 本头文件故意【只】放引脚/尺寸/常量,【不】include display/hal/gui。
//          这样 starboard_config 没有依赖,其它 component 各自 include 它,
//          才能避免循环依赖。全局对象(display/hal/...)的定义放在各自 component 里。
//
// 📌 引脚来源说明:
//    - 屏幕 SPI 一组: 沿用当前 hello-world 已验证能跑通的引脚(CS=10/DC=8/RST=7/BUSY=9,
//      SCK=12/MOSI=11 为 S3 默认 SPI)。这组【已验证】,一般不用改。
//    - 其它引脚(按键/ADC/SD/蜂鸣器/I2C): S3 与 LiClock 的 ESP32-Solo-1 引脚完全不同,
//      下面给的是【占位值,标记 ⚠️占位待改】,请按你的实际接线修改。
//
// ⚠️ S3 引脚禁忌(选引脚时务必避开):
//    - GPIO26~32: 被 Octal PSRAM 占用
//    - GPIO33~37: 被 SPI Flash 占用(部分模组)
//    - GPIO0/3/45/46: strap 引脚,上电电平影响启动模式,慎用
//    - GPIO19/20: USB D-/D+(若用 USB 烧录/供电则不可挪用)
//    - 深睡唤醒按键: 必须是 RTC GPIO(S3 的 GPIO0~21 多为 RTC GPIO)
// =============================================================================

// ----------------------------- 屏幕 SPI --------------------------------------
// 这组沿用 hello-world,已验证可用。如改线,同步改 main.cpp 构造函数。
#define CONFIG_SPI_MOSI 11
#define CONFIG_SPI_SCK  12
#define CONFIG_SPI_MISO 13 // 屏幕一般不读,保留默认
#define CONFIG_SPI_CS   10
#define CONFIG_PIN_DC   8
#define CONFIG_PIN_RST  7
#define CONFIG_PIN_BUSY 9

// 4.2 寸三色屏 GDEY042Z98
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 300

// ----------------------------- 按键(拨轮三键)  -----------------------
// S3 安全的 RTC GPIO(0~21 区间)。LiClock 用 34/35/39,S3 不可用,这里改占位。
// 选了 GPIO0/1/2:均在 RTC GPIO 范围,可做深睡唤醒。
// ⚠️ GPIO0 是 strap 引脚(上电需为高),若你的按键按下拉低会进下载模式,请换其它。
//    推荐实测后改成无 strap 冲突的引脚(如 4/5/6 等)。
#define PIN_BUTTONL 4 //  左键(数字-/返回)
#define PIN_BUTTONC 5 //   中键(确认)
#define PIN_BUTTONR 6 //   右键(数字+)
// 按键有效电平:true=按下读到低电平(常见接法),false=按下读到高电平。
// LiClock 会自动检测;S3 上先用固定值,实测后改。
#define BUTTON_ACTIVE_LOW true

// ----------------------------- 电池/充电 ⚠️占位待改 ---------------------------
// LiClock: PIN_ADC=33(S3 不可用),充电=26(S3 不可用,且26被PSRAM占)。
// S3 ADC1 通道对应 GPIO1~10。⚠️ 这里和按键占位可能冲突,务必按实际接线改。
#define PIN_ADC       4 // ⚠️占位待改  电池分压 ADC(ADC1 通道)
#define PIN_CHARGING  5 // ⚠️占位待改  充电状态输入(低=充电中)
// 电池分压换算: LiClock 用 adc*7230/4096(12bit)。改你的分压电阻后调系数。
#define BATTERY_ADC_MAX 4095
#define BATTERY_SCALE   7230 // adc * BATTERY_SCALE / BATTERY_ADC_MAX ≈ 毫伏

// ----------------------------- 蜂鸣器 ⚠️占位待改 ------------------------------
#define PIN_BUZZER 6 // ⚠️占位待改  LEDC 驱动无源蜂鸣器

// ----------------------------- SD 卡 ⚠️占位待改(暂可不用) ---------------------
// 若暂不用 SD,这些不会被引用(SD 相关代码暂不移植)。填好引脚后阶段后期启用。
#define PIN_SD_CS       10 // ⚠️占位待改(注意勿和屏幕 CS=10 冲突,实际接线改)
#define PIN_SD_MOSI     11
#define PIN_SD_SCLK     12
#define PIN_SD_MISO     13
#define PIN_SD_CARDDETECT -1 // -1 表示无卡检测
#define PIN_SDVDD_CTRL    -1 // SD 电源控制,-1 表示无

// ----------------------------- I2C(传感器) ⚠️占位待改 -------------------------
// 本工程暂无传感器(AHT/BMP/SGP),先留定义,后期加传感器时用。
#define PIN_SDA 8 // ⚠️占位待改(注意勿和屏幕 DC=8 冲突,实际接线改)
#define PIN_SCL 9 // ⚠️占位待改(注意勿和屏幕 BUSY=9 冲突,实际接线改)

// ----------------------------- 全局行为常量 ----------------------------------
// 默认配置 JSON(首次开机写入 /config.json)。键名沿用 LiClock。
#define DEFAULT_CONFIG "{\"p1\":\"116.3975,39.9091\",\"p2\":\"15\",\"p3\":\"1\",\"p4\":\"23:30\",\"p5\":\"05:00\",\"p6\":\"\",\"p7\":\"\",\"p8\":\"0\"}"

// 时区: 东八区
#define TIMEZONE "CST-8"

// NTP 同步间隔选项表(分钟),索引对应 settings 的 p1 值
// 0:禁用 1:2h 2:4h 3:6h 4:12h 5:24h 6:36h 7:48h

#endif // STARBOARD_CONFIG_H
