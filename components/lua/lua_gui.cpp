// lua_gui —— Lua 绑定: GUI(消息框/菜单)
//
// 移植自 LiClock/src/lua/modules/lua_gui.cpp(172行,9函数)。
// 跳过 drawLBM(需 LittleFS LBM 支持)和 fileDialog(需 LittleFS)。
// menu 接收 Lua table → 转换为 menu_item 数组。

#include "starboard_lua.h"
#include <starboard_gui.h>
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

static const luaL_Reg _lualib[] = {
    {"waitLongPress", gui_waitLongPress},
    {"autoIndentDraw", gui_autoIndentDraw},
    {"drawWindowsWithTitle", gui_drawWindowsWithTitle},
    {"msgbox", gui_msgbox},
    {"msgbox_yn", gui_msgbox_yn},
    {"msgbox_number", gui_msgbox_number},
    {"menu", gui_menu},
    {NULL, NULL},
};

extern "C" int luaopen_gui(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}