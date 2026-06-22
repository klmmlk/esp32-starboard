# Adafruit_BusIO(Adafruit_GFX 的底层依赖,必须装)
# 源码:https://github.com/adafruit/Adafruit_BusIO
# 操作:把该仓库根目录下的 *.cpp / *.h 全部拷到本目录
file(GLOB BUSIO_SRCS "*.cpp")
idf_component_register(
    SRCS ${BUSIO_SRCS}
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32
)
