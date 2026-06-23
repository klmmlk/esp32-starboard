# main 组件
# - arduino-esp32: Arduino 运行时
# - GxEPD2: 墨水屏驱动(内部已 REQUIRES Adafruit_GFX / Adafruit_BusIO)
# - starboard_config: 全局引脚/常量定义
# - ArduinoJson: 配置 JSON 存取(阶段1 起 HAL 用)
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 GxEPD2 starboard_config starboard_hal
)
