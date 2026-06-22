# Adafruit_GFX(GxEPD2 的图形/字体库)
# 源码:https://github.com/adafruit/Adafruit-GFX-Library
# 操作:把仓库根目录下的 *.cpp / *.c / *.h 以及 Fonts/ 目录拷到本目录
#
# ⚠️ 用非递归 GLOB:不要把 fontconvert/、examples/ 这些带 main() 的子目录编译进来,
#    否则会报 "multiple definition of main"。
file(GLOB GFX_SRCS "*.cpp" "*.c")
idf_component_register(
    SRCS ${GFX_SRCS}
    INCLUDE_DIRS "."
    REQUIRES Adafruit_BusIO arduino-esp32
)
