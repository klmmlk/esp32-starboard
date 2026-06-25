# starboard_display: 三色墨水屏 + 中文字体 封装层
# - GxEPD2: 屏驱动(内部已 REQUIRES Adafruit_GFX / Adafruit_BusIO)
# - U8g2_for_Adafruit_GFX: 中文字体渲染
# - starboard_config: 屏幕引脚宏(顶层 #define,非 Kconfig)
# - arduino-esp32: Arduino runtime / SPI
#
# ⚠️ 刻意不依赖 starboard_hal/gui(显示层独立),GxEPD2/U8g2 经 PUBLIC 传递给上层(main)。
file(GLOB DISPLAY_SRCS "starboard_display.cpp" "*.cpp")
idf_component_register(
    SRCS ${DISPLAY_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES GxEPD2 U8g2_for_Adafruit_GFX starboard_config arduino-esp32
)
