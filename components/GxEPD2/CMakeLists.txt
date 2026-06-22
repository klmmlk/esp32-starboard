# GxEPD2 墨水屏驱动库
# 源码:https://github.com/ZinggJM/GxEPD2
# 操作:把 GxEPD2 仓库里 src/ 目录的内容,全部拷到本目录的 src/ 下
#        (即本目录结构应为:components/GxEPD2/src/GxEPD2_3C.h ...)
#
# ⚠️ GxEPD2 新版按厂家把驱动拆进 src/ 的子目录(epd/、gdey3c/、it8951/ …),
#    你用的 GxEPD2_420c_GDEY042Z98 就在 gdey3c/ 里,必须用 GLOB_RECURSE 递归收录。
file(GLOB_RECURSE GXEPD2_SRCS CONFIG_DEPENDS "src/*.cpp")
idf_component_register(
    SRCS ${GXEPD2_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES Adafruit_GFX Adafruit_BusIO arduino-esp32
)
