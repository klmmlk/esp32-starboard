# main 组件
# - arduino-esp32: Arduino 运行时
# - starboard_display: 三色屏 + 中文字体(内部 REQUIRES GxEPD2/U8g2/Adafruit_GFX,经 PUBLIC 传递)
# - starboard_config: 全局引脚/常量定义
# - starboard_hal: HAL(按键/WiFi/NTP/深睡)
# - ArduinoJson: 配置 JSON 存取(阶段1 起 HAL 用)
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 starboard_config starboard_hal starboard_display starboard_gui
)
