# U8g2_for_Adafruit_GFX(在 Adafruit_GFX 上显示 u8g2 字体,本项目用来显示中文)
# 源码: https://github.com/olikraus/U8g2_for_Adafruit_GFX
# 操作: 把 LiClock/lib/U8g2_for_Adafruit_GFX/src/ 的 4 个文件拷到本目录
# 注意: u8g2_fonts 是 .c 文件,要和 .cpp 一起收录
# ⚠️ 用 file(GLOB) 收录所有 .c/.cpp。GLOB 不自动跟踪文件增删：
#    新增/删除字体 .c 后必须 `idf.py reconfigure`（或 fullclean）才会重新扫描，
#    否则 ninja 仍沿用旧的源文件清单，会去编译已删除的 .c（报“幽灵”错误）。
#    不能加 CONFIGURE_DEPENDS：IDF 在 script mode 解析组件 CMakeLists 提取 REQUIRES 时
#    会报 "CONFIGURE_DEPENDS is invalid for script and find package modes"。
file(GLOB U8G2_SRCS "simhei16.c" "*.cpp" "*.c")
idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "."
    REQUIRES Adafruit_GFX arduino-esp32
)

# 第三方库源码(u8g2_fonts.c 等)不改动,避免被项目默认的 -Werror 卡住。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
