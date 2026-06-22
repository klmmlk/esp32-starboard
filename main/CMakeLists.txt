# main 组件:arduino-esp32 提供 Arduino 运行时,GxEPD2 提供墨水屏驱动
# (GxEPD2 内部已 REQUIRES 了 Adafruit_GFX / Adafruit_BusIO,会自动传递)
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32 GxEPD2
)
