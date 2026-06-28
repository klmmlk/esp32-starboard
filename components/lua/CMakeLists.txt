# components/lua —— Lua 5.4 嵌入式运行时
#
# 移植自 LiClock/lib/lua(Lua5.4 官方源码)。配置:
#   - LUA_32BITS=1 → float(32位) + int(32位),省内存
#   - 排除 lua.c/luac.c(独立解释器/编译器,非嵌入用途)
#   - 压制大量 -Werror 警告(Lua C 源码标准写法,无需改)
#
# ⚠️ liolib.c 的 fopen/fread 默认走 POSIX,后续需重定向到 LittleFS(VFS)。
# ⚠️ loslib.c 的 os_tmpname 用 POSIX mkstemp;ldblib.c hook 调 debug 在 xtensa 上 OK。

file(GLOB LUA_SRCS "lua_http.cpp" "lua_display.cpp" "lua_app_wrapper.cpp" "lua_data.cpp" "linit.c" "starboard_lua.cpp" "lua_hal.cpp" "lua_webserver.cpp" "lua_gui.cpp" "*.c")
# 排除独立工具(lua 解释器 + luac 编译器,非嵌入用途)
list(REMOVE_ITEM LUA_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/lua.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/luac.c"
)

idf_component_register(
    SRCS ${LUA_SRCS} starboard_lua.cpp lua_display.cpp lua_hal.cpp lua_gui.cpp lua_appmanager.cpp lua_http.cpp lua_webserver.cpp lua_app_wrapper.cpp lua_sys.cpp
    INCLUDE_DIRS "include"
    REQUIRES arduino-esp32 starboard_config starboard_display starboard_hal starboard_app starboard_gui esp_http_client
)

# Lua 配置:32位浮点+整数;压制 Lua C 源码的 -Werror 警告
#   - Wno-unused-but-set-variable: lparser.c 频繁出现
#   - Wno-implicit-fallthrough: liolib.c 等标准分支
#   - Wno-sign-compare: Lua 内部比较
#   - Wno-format: lmathlib.c 用 LUA_NUMBER_FMT 宏传入 float,xtensa gcc 的 printf 期望 double
target_compile_options(${COMPONENT_LIB} PRIVATE
    -DLUA_32BITS=1
    -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-format
)