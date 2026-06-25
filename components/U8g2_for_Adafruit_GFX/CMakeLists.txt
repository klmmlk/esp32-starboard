# U8g2_for_Adafruit_GFX(在 Adafruit_GFX 上显示 u8g2 字体,本项目用来显示中文)
# 源码: https://github.com/olikraus/U8g2_for_Adafruit_GFX
# 操作: 把 LiClock/lib/U8g2_for_Adafruit_GFX/src/ 的 4 个文件拷到本目录
# 注意: u8g2_fonts 是 .c 文件,要和 .cpp 一起收录
file(GLOB U8G2_SRCS "simhei16.c" "*.cpp" "*.c")
idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "."
    REQUIRES Adafruit_GFX arduino-esp32
)

# 第三方库源码(u8g2_fonts.c 等)不改动,避免被项目默认的 -Werror 卡住。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
