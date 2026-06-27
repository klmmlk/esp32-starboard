// lua_gui —— Lua 绑定: GUI(消息框/菜单)
//
// 移植自 LiClock/src/lua/modules/lua_gui.cpp(172行,9函数)。
// 跳过 drawLBM(需 LittleFS LBM 支持)和 fileDialog(需 LittleFS)。
// menu 接收 Lua table → 转换为 menu_item 数组。

#include "starboard_lua.h"
#include <starboard_gui.h>
#include <starboard_display.h>
#include <Arduino.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int gui_waitLongPress(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数个数不符");
    int btn = luaL_checkinteger(L, 1);
    lua_pushboolean(L, GUI::waitLongPress(btn));
    return 1;
}

// 阻塞等待任意按键,返回 1=左 2=中 3=右。
// 用非阻塞 tryGetKey 轮询,每轮检查停止标志——这样长按中键强停时能立即跳出
// (若直接用 GUI::waitKey,会死等新按键事件,无法被强停)。
static int gui_waitKey(lua_State *L)
{
    for (;;)
    {
        if (luaStopRequested())
            luaL_error(L, "stopped by user (长按中键强制停止)");
        int k = GUI::tryGetKey();
        if (k != 0)
        {
            lua_pushinteger(L, k);
            return 1;
        }
        delay(10);
    }
}

// 阻塞等待【指定】按键被按下(忽略其他键)。target: 1=左 2=中 3=右。
static int gui_waitButton(lua_State *L)
{
    int target = (int)luaL_checkinteger(L, 1);
    for (;;)
    {
        if (luaStopRequested())
            luaL_error(L, "stopped by user (长按中键强制停止)");
        int k = GUI::tryGetKey();
        if (k != 0 && k == target)
            return 0;
        delay(10);
    }
}

// 非阻塞读按键:有键返回 1/2/3,无键返回 0(配合 millis 做空闲超时)。
static int gui_tryGetKey(lua_State *L)
{
    if (luaStopRequested())
        luaL_error(L, "stopped by user (长按中键强制停止)");
    lua_pushinteger(L, GUI::tryGetKey());
    return 1;
}

static int gui_autoIndentDraw(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    int max_x = (int)lua_tointeger(L, 2);
    if (max_x <= 0) max_x = SCREEN_WIDTH;
    int start_x = 2;
    if (lua_gettop(L) >= 3)
        start_x = (int)luaL_checkinteger(L, 3);
    GUI::autoIndentDraw(str, max_x, start_x);
    return 0;
}

static int gui_drawWindowsWithTitle(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    int16_t x = (int16_t)lua_tointeger(L, 2);
    int16_t y = (int16_t)lua_tointeger(L, 3);
    int16_t w = (int16_t)lua_tointeger(L, 4);
    int16_t h = (int16_t)lua_tointeger(L, 5);
    if (w <= 0) w = SCREEN_WIDTH;
    if (h <= 0) h = SCREEN_HEIGHT;
    GUI::drawWindowsWithTitle(title, x, y, w, h);
    return 0;
}

static int gui_msgbox(lua_State *L)
{
    if (lua_gettop(L) < 1)
        return luaL_error(L, "参数个数不符");
    const char *title = luaL_checkstring(L, 1);
    const char *msg = lua_tostring(L, 2);
    GUI::msgbox(title, msg);
    return 0;
}

static int gui_msgbox_yn(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *msg = lua_tostring(L, 2);
    const char *yes = lua_tostring(L, 3);
    const char *no = lua_tostring(L, 4);
    lua_pushboolean(L, GUI::msgbox_yn(title, msg, yes, no));
    return 1;
}

static int gui_msgbox_number(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    uint16_t digits = (uint16_t)lua_tointeger(L, 2);
    if (digits == 0) digits = 1;
    int pre_value = (int)lua_tointeger(L, 3);
    lua_pushinteger(L, GUI::msgbox_number(title, digits, pre_value));
    return 1;
}

static int gui_menu(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    // 第二个参数: table of strings
    int len = (int)lua_rawlen(L, 2);

    menu_item *options = new menu_item[len + 1];
    for (int i = 0; i < len; i++)
    {
        lua_pushinteger(L, i + 1);
        lua_gettable(L, -2);
        options[i].title = new char[64];
        const char *s = lua_tostring(L, -1);
        strncpy((char *)options[i].title, s ? s : "", 63);
        ((char *)options[i].title)[63] = '\0';
        options[i].icon = nullptr;
        lua_pop(L, 1);
    }
    options[len].title = nullptr;
    options[len].icon = nullptr;

    int ret = GUI::menu(title, options);

    for (int i = 0; i < len; i++)
        delete[] (char *)options[i].title;
    delete[] options;

    lua_pushinteger(L, ret + 1); // Lua 索引从 1 开始
    return 1;
}

static int gui_drawLBM(lua_State *L)
{
    int16_t x = (int16_t)luaL_checkinteger(L, 1);
    int16_t y = (int16_t)luaL_checkinteger(L, 2);
    const char *filename = luaL_checkstring(L, 3);
    int color = (int)luaL_checkinteger(L, 4);
    GUI::drawLBM(x, y, filename, (uint16_t)color);
    return 0;
}

// 解码hex字符串(小端宽高头+像素)为位图,返回分配好的buf(需free)
static uint8_t *_decodeHexBM(const char *hex, uint16_t *outW, uint16_t *outH)
{
    size_t len = strlen(hex);
    uint16_t w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        char byteStr[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        uint8_t byte = (uint8_t)strtol(byteStr, NULL, 16);
        if (i < 2) w |= byte << (i * 8);
        else        h |= byte << ((i - 2) * 8);
    }
    *outW = w; *outH = h;
    size_t expectedLen = ((w + 7) / 8) * h;
    if (len < 8 || (len - 8) < expectedLen * 2) return NULL;
    uint8_t *img = (uint8_t *)malloc(expectedLen);
    if (!img) return NULL;
    for (size_t i = 0; i < expectedLen; i++) {
        char byteStr[3] = {hex[8 + i * 2], hex[8 + i * 2 + 1], 0};
        img[i] = (uint8_t)strtol(byteStr, NULL, 16);
    }
    return img;
}

static int gui_drawBWBM(lua_State *L)
{
    int16_t x = (int16_t)luaL_checkinteger(L, 1);
    int16_t y = (int16_t)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    const char *hex = luaL_checkstring(L, 5);
    int color = (int)luaL_checkinteger(L, 6);
    uint16_t bw = 0, bh = 0;
    uint8_t *img = _decodeHexBM(hex, &bw, &bh);
    if (!img) return luaL_error(L, "invalid hex data");
    display.drawXBitmap(x, y, img, bw, bh, (uint16_t)color);
    free(img);
    return 0;
}

static int gui_draw3ColorBM(lua_State *L)
{
    int16_t x = (int16_t)luaL_checkinteger(L, 1);
    int16_t y = (int16_t)luaL_checkinteger(L, 2);
    // w/h 参数实际上可从hex头解析,这里不强制校验
    const char *hexBw = luaL_checkstring(L, 5);
    const char *hexRed = luaL_checkstring(L, 6);
    // 空串 = 该层不绘制(纯红图黑白层留空,纯黑图红色层留空)
    uint16_t bw = 0, bh = 0, rw = 0, rh = 0;
    uint8_t *blackImg = (hexBw[0] ? _decodeHexBM(hexBw, &bw, &bh) : nullptr);
    uint8_t *redImg = (hexRed[0] ? _decodeHexBM(hexRed, &rw, &rh) : nullptr);
    if (!blackImg && !redImg)
        return luaL_error(L, "invalid hex data");
    if (blackImg)
    {
        display.drawXBitmap(x, y, blackImg, bw, bh, COL_NORMAL);
        free(blackImg);
    }
    if (redImg)
    {
        display.drawXBitmap(x, y, redImg, rw, rh, COL_ALERT);
        free(redImg);
    }
    return 0;
}

static const luaL_Reg _lualib[] = {
    {"waitLongPress", gui_waitLongPress},
    {"waitKey", gui_waitKey},
    {"waitButton", gui_waitButton},
    {"tryGetKey", gui_tryGetKey},
    {"autoIndentDraw", gui_autoIndentDraw},
    {"drawWindowsWithTitle", gui_drawWindowsWithTitle},
    {"msgbox", gui_msgbox},
    {"msgbox_yn", gui_msgbox_yn},
    {"msgbox_number", gui_msgbox_number},
    {"menu", gui_menu},
    {"drawLBM", gui_drawLBM},
    {"drawBWBM", gui_drawBWBM},
    {"draw3ColorBM", gui_draw3ColorBM},
    {NULL, NULL},
};

extern "C" int luaopen_gui(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}