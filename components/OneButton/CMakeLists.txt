# OneButton 按键库(短按/双击/长按/多击事件)
# 源码: https://github.com/mathertel/OneButton
# 操作: 把 LiClock/lib/OneButton/src/ 的 OneButton.cpp/.h 拷到本目录
file(GLOB ONEBUTTON_SRCS "*.cpp")
idf_component_register(
    SRCS ${ONEBUTTON_SRCS}
    INCLUDE_DIRS "."
    REQUIRES arduino-esp32
)

# 第三方库源码不改动,避免被项目默认的 -Werror 卡住。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
