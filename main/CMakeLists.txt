# main 组件
# - arduino-esp32: Arduino 运行时
# - starboard_config: 全局引脚/常量定义
# - starboard_hal: HAL(按键/WiFi/NTP/深睡)
# - starboard_display: 三色屏 + 中文字体(内部 REQUIRES GxEPD2/U8g2/Adafruit_GFX,经 PUBLIC 传递)
# - starboard_gui: GUI(消息框/菜单)
# - starboard_app: AppManager 应用框架(阶段3)
# - apps/*.cpp: 内置 App(clock/settings/oobe...),注册进 appManager
idf_component_register(
    SRCS "main.cpp" "apps/appClock.cpp" "apps/appSettings.cpp" "apps/appOOBE.cpp" "apps/appOTA.cpp" "apps/apps.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 starboard_config starboard_hal starboard_display starboard_gui starboard_app esp_http_client app_update
)
