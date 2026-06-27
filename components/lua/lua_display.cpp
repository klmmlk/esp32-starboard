// lua_display —— Lua 绑定: 三色屏绘图
//
// 移植自 LiClock/src/lua/modules/lua_display.cpp(407行,26函数)。
// 适配: 用 starboard_display/u8g2(非 LiClock 的 u8g2Fonts);
//       全刷 firstPage/nextPage 替代 clearScreen+display;
//       用 COL_NORMAL/COL_ALERT/COL_BG 替代硬编码黑白;
//       无 drawLBM(LittleFS 未就绪);
//       setFont(fontName) 通过字体名选择 20 种字体(含中文 wqy12/wqy16)。

#include "starboard_lua.h"

#include "starboard_lua.h"
#include <starboard_display.h>
#include <Arduino.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------------------- 绘图基元 ----------------------

static int display_beginFrame(lua_State *L)
{
    // 开始一帧:设全窗,之后绘制都进入整屏缓冲区(_black_buffer/_color_buffer)
    display.setFullWindow();
    return 0;
}

static int display_endFrame(lua_State *L)
{
    // 结束一帧:直接把整屏缓冲区一次性刷到屏幕(非分页模式)
    // display.display() 内部 = writeImage(整屏) + refresh + powerOff
    display.display(false); // false = 全屏刷新(非局部)
    return 0;
}

static int display_clearScreen(lua_State *L)
{
    // 纯缓冲操作:用颜色填充缓冲区(不刷屏)
    int color = (lua_gettop(L) >= 1) ? (int)lua_tointeger(L, 1) : COL_BG;
    display.fillScreen((uint16_t)color);
    return 0;
}

static int display_fillScreen(lua_State *L)
{
    int color = (lua_gettop(L) >= 1) ? (int)lua_tointeger(L, 1) : COL_BG;
    display.fillScreen((uint16_t)color);
    return 0;
}

static int display_drawPixel(lua_State *L)
{
    if (lua_gettop(L) != 3)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int color = luaL_checkinteger(L, 3);
    display.drawPixel(x, y, (uint16_t)color);
    return 0;
}

static int display_drawLine(lua_State *L)
{
    if (lua_gettop(L) != 5)
        return luaL_error(L, "参数个数不符");
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    display.drawLine(x0, y0, x1, y1, (uint16_t)color);
    return 0;
}

static int display_drawRect(lua_State *L)
{
    if (lua_gettop(L) != 5)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    display.drawRect(x, y, w, h, (uint16_t)color);
    return 0;
}

static int display_fillRect(lua_State *L)
{
    if (lua_gettop(L) != 5)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);
    display.fillRect(x, y, w, h, (uint16_t)color);
    return 0;
}

static int display_drawCircle(lua_State *L)
{
    if (lua_gettop(L) != 4)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int r = luaL_checkinteger(L, 3);
    int color = luaL_checkinteger(L, 4);
    display.drawCircle(x, y, r, (uint16_t)color);
    return 0;
}

static int display_fillCircle(lua_State *L)
{
    if (lua_gettop(L) != 4)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int r = luaL_checkinteger(L, 3);
    int color = luaL_checkinteger(L, 4);
    display.fillCircle(x, y, r, (uint16_t)color);
    return 0;
}

static int display_drawTriangle(lua_State *L)
{
    if (lua_gettop(L) != 7)
        return luaL_error(L, "参数个数不符");
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    int x2 = luaL_checkinteger(L, 5);
    int y2 = luaL_checkinteger(L, 6);
    int color = luaL_checkinteger(L, 7);
    display.drawTriangle(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    return 0;
}

static int display_fillTriangle(lua_State *L)
{
    if (lua_gettop(L) != 7)
        return luaL_error(L, "参数个数不符");
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    int x2 = luaL_checkinteger(L, 5);
    int y2 = luaL_checkinteger(L, 6);
    int color = luaL_checkinteger(L, 7);
    display.fillTriangle(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    return 0;
}

static int display_drawRoundRect(lua_State *L)
{
    if (lua_gettop(L) != 6)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int color = luaL_checkinteger(L, 6);
    display.drawRoundRect(x, y, w, h, r, (uint16_t)color);
    return 0;
}

static int display_fillRoundRect(lua_State *L)
{
    if (lua_gettop(L) != 6)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int color = luaL_checkinteger(L, 6);
    display.fillRoundRect(x, y, w, h, r, (uint16_t)color);
    return 0;
}

// ---------------------- 字体选择 ----------------------

// 字体名 → 字体指针 映射表
// 命名格式: [风格][宽x高], 风格 t=透明背景, B=粗体, n=窄体
static const struct { const char *name; const uint8_t *font; } _font_map[] = {
    // 中文（含 ASCII）
    {"wqy12",       u8g2_font_wqy12_t_gb2312},
    {"wqy16",       u8g2_font_wqy16_t_gb2312},
    // 英文等宽 透明背景
    {"4x6",         u8g2_font_4x6_tf},
    {"5x7",         u8g2_font_5x7_tf},
    {"5x8",         u8g2_font_5x8_tf},
    {"6x10",        u8g2_font_6x10_tf},
    {"6x12",        u8g2_font_6x12_tf},
    {"6x13",        u8g2_font_6x13_tf},
    {"7x13",        u8g2_font_7x13_tf},
    {"7x14",        u8g2_font_7x14_tf},
    {"8x13",        u8g2_font_8x13_tf},
    {"9x15",        u8g2_font_9x15_tf},
    {"9x18",        u8g2_font_9x18_tf},
    {"10x20",       u8g2_font_10x20_tf},
    // 粗体 (B)
    {"bold6x13",    u8g2_font_6x13B_tf},
    {"bold7x13",    u8g2_font_7x13B_tf},
    {"bold8x13",    u8g2_font_8x13B_tf},
    {"bold9x15",    u8g2_font_9x15B_tf},
    // 窄体 (n = narrow)
    {"narrow6x12",  u8g2_font_6x12_tn},
    {"narrow6x13",  u8g2_font_6x13_tn},
    {"narrow7x13",  u8g2_font_7x13_tn},
    {"narrow8x13",  u8g2_font_8x13_tn},
};
static const int _font_map_count = sizeof(_font_map) / sizeof(_font_map[0]);

static int display_setFont(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < _font_map_count; i++) {
        if (strcmp(name, _font_map[i].name) == 0) {
            u8g2.setFont(_font_map[i].font);
            return 0;
        }
    }
    return luaL_error(L, "未知字体: %s", name);
}

// ---------------------- 文字输出 ----------------------

static int display_setCursor(lua_State *L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L, "参数个数不符");
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    display.setCursor(x, y);
    u8g2.setCursor(x, y);
    return 0;
}

static int display_setTextColor(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int color = luaL_checkinteger(L, 1);
    display.setTextColor((uint16_t)color);
    u8g2.setForegroundColor((uint16_t)color);
    return 0;
}

static int display_setBackgroundColor(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int color = luaL_checkinteger(L, 1);
    u8g2.setBackgroundColor((uint16_t)color);
    return 0;
}

static int display_print(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    display.print(str);
    return 0;
}

static int display_u8g2Print(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    u8g2.print(str);
    return 0;
}

static int display_getCursorX(lua_State *L)
{
    lua_pushinteger(L, display.getCursorX());
    return 1;
}

static int display_getCursorY(lua_State *L)
{
    lua_pushinteger(L, display.getCursorY());
    return 1;
}

static int display_u8g2GetCursorX(lua_State *L)
{
    lua_pushinteger(L, u8g2.getCursorX());
    return 1;
}

static int display_u8g2GetCursorY(lua_State *L)
{
    lua_pushinteger(L, u8g2.getCursorY());
    return 1;
}

// ---------------------- 注册表 ----------------------

static const luaL_Reg _lualib[] = {
    {"beginFrame", display_beginFrame},
    {"endFrame", display_endFrame},
    {"clearScreen", display_clearScreen},
    {"fillScreen", display_fillScreen},
    {"setFont", display_setFont},
    {"drawPixel", display_drawPixel},
    {"drawLine", display_drawLine},
    {"drawRect", display_drawRect},
    {"fillRect", display_fillRect},
    {"drawCircle", display_drawCircle},
    {"fillCircle", display_fillCircle},
    {"drawTriangle", display_drawTriangle},
    {"fillTriangle", display_fillTriangle},
    {"drawRoundRect", display_drawRoundRect},
    {"fillRoundRect", display_fillRoundRect},
    {"setCursor", display_setCursor},
    {"setTextColor", display_setTextColor},
    {"setBackgroundColor", display_setBackgroundColor},
    {"print", display_print},
    {"u8g2Print", display_u8g2Print},
    {"getCursorX", display_getCursorX},
    {"getCursorY", display_getCursorY},
    {"u8g2GetCursorX", display_u8g2GetCursorX},
    {"u8g2GetCursorY", display_u8g2GetCursorY},
    {NULL, NULL},
};

extern "C" int luaopen_display(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}