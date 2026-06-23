// ESP-IDF 版墨水屏测试程序(阶段0 临时验证程序)
// 阶段3 起本文件会改成 app_main → hal.init() → 起 appManager 任务,
// 屏幕实例也会移到 starboard_display 组件。当前只为验证阶段0 的工程结构能编译。
//
// 引脚说明:CS=10 / SCK=12 / MOSI=11 / MISO=13 是 ESP32-S3 默认 SPI,
// 和原 Arduino 工程完全一致,所以这里直接用默认 SPI 即可。
// ⚠️ 引脚值统一从 starboard_config 取,改引脚只改那个头文件。

#include <Arduino.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <starboard_config.h> // 阶段0:验证该组件能被 include

// 屏幕实例(引脚从 starboard_config 取)
GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT> display(
    GxEPD2_420c_GDEY042Z98(/*CS=*/CONFIG_SPI_CS, /*DC=*/CONFIG_PIN_DC, /*RST=*/CONFIG_PIN_RST, /*BUSY=*/CONFIG_PIN_BUSY));

void helloWorld()
{
    const char str1[] = "Welcome to";
    const char str2[] = "www.JokerIn.Icu!";

    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);

    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(str1, 0, 0, &tbx, &tby, &tbw, &tbh);
    uint16_t x = ((display.width() - tbw) / 2) - tbx;
    uint16_t y = ((display.height() - tbh) / 2) - tby;

    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(x, y);
        display.print(str1);
        display.setTextColor(GxEPD_RED);
        display.setCursor(x - 45, y + 30);
        display.print(str2);
    } while (display.nextPage());
}

void setup()
{
    display.init(115200); // 参数是串口日志波特率,会顺带初始化 Serial
    helloWorld();
    display.hibernate();  // 刷完进入深度休眠,省电
}

void loop() {}

// ESP-IDF 入口:先启动 Arduino 运行时,再跑 setup/loop
extern "C" void app_main()
{
    initArduino();
    setup();
    while (true)
    {
        loop();
        delay(1); // loop 为空时让出 CPU,避免空转占用整个核
    }
}
