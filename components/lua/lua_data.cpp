// lua_data —— Lua 绑定: 数据持久化(每 App 独立 data.kv,键值对文本文件)
//
// 数据文件: /littlefs/apps/<当前App名>/data.kv, 每行 "key=value\n"
// value 统一存字符串, load 时按 default 类型(数字/字符串)转换返回。
// 当前 App 名由 luaSetCurrentApp 设置(pollRunRequest / LuaApp::setup 调用)。

#include "starboard_lua.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 构建当前 App 的数据文件路径,返回 false 表示无 App 上下文
static bool buildDataPath(char *out, size_t outSize)
{
    const char *app = luaGetCurrentApp();
    if (!app || !app[0])
        return false;
    snprintf(out, outSize, "/littlefs/apps/%s/data.kv", app);
    return true;
}

// 读整个文件到行数组
static std::vector<std::string> readAllLines(const char *path)
{
    std::vector<std::string> lines;
    FILE *f = fopen(path, "r");
    if (!f)
        return lines;
    char buf[512];
    while (fgets(buf, sizeof(buf), f))
        lines.push_back(buf);
    fclose(f);
    return lines;
}

static int data_save(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    // value → 字符串(数字也转字符串)
    char numbuf[32];
    const char *valstr;
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        snprintf(numbuf, sizeof(numbuf), "%g", lua_tonumber(L, 2));
        valstr = numbuf;
    }
    else
        valstr = luaL_checkstring(L, 2);

    char path[160];
    if (!buildDataPath(path, sizeof(path)))
        return luaL_error(L, "no app context (data.save)");

    std::string keyeq = std::string(key) + "=";
    std::vector<std::string> lines = readAllLines(path);
    bool found = false;
    for (auto &l : lines)
    {
        if (l.compare(0, keyeq.size(), keyeq) == 0)
        {
            l = keyeq + valstr + "\n";
            found = true;
            break;
        }
    }
    if (!found)
        lines.push_back(keyeq + valstr + "\n");

    FILE *f = fopen(path, "w");
    if (!f)
        return luaL_error(L, "cannot write data file");
    for (auto &l : lines)
        fputs(l.c_str(), f);
    fclose(f);
    return 0;
}

static int data_load(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    luaL_checkany(L, 2); // default value

    char path[160];
    if (!buildDataPath(path, sizeof(path)))
    {
        lua_pushvalue(L, 2); // 无上下文,返回默认
        return 1;
    }

    std::string keyeq = std::string(key) + "=";
    std::vector<std::string> lines = readAllLines(path);
    for (auto &l : lines)
    {
        if (l.compare(0, keyeq.size(), keyeq) == 0)
        {
            std::string val = l.substr(keyeq.size());
            if (!val.empty() && val.back() == '\n')
                val.pop_back();
            // 按 default 类型转换
            if (lua_type(L, 2) == LUA_TNUMBER)
                lua_pushnumber(L, strtod(val.c_str(), nullptr));
            else
                lua_pushlstring(L, val.c_str(), val.size());
            return 1;
        }
    }
    // 没找到 key,返回默认值
    lua_pushvalue(L, 2);
    return 1;
}

static const luaL_Reg _lualib[] = {
    {"save", data_save},
    {"load", data_load},
    {NULL, NULL},
};

extern "C" int luaopen_data(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}
