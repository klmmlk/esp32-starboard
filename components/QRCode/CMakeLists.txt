# QRCode 二维码生成库
# 源码: https://github.com/ricmoo/QRCode
# 操作: 把 LiClock/lib/QRCode/src/ 的 qrcode.c/.h 拷到本目录
# 纯 C 库,不依赖 Arduino
idf_component_register(
    SRCS "qrcode.c"
    INCLUDE_DIRS "."
)

# qrcode.c 含 #pragma mark(Apple/Xcode 专用,GCC 不识别)和 type-limits 比较,
# IDF 默认 -Werror=all 把它们当错误。
# ⚠️ 裸 -Wno-error 压不住 GCC 里 -Werror=<具体类型>,必须用具体的 -Wno-<type> 从根上消除。
# 第三方源码不改动,仅本组件关掉这两类警告:
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unknown-pragmas -Wno-type-limits)
