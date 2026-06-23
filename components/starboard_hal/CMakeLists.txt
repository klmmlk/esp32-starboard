# starboard_hal: 硬件抽象层
# 按键/电源/WiFi配网/NTP/深睡/电压/蜂鸣器
# ⚠️ 刻意不依赖 display/gui(保持底层纯粹),配网等 UI 交互由上层 App 处理
file(GLOB HAL_SRCS "*.cpp")
idf_component_register(
    SRCS ${HAL_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES arduino-esp32 OneButton starboard_config
)
