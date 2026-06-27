// lua_http —— Lua 绑定: HTTP 请求(IDF 原生 esp_http_client)
//
// 不用 Arduino HTTPClient(避免 NetworkClientSecure SSL 链接问题),
// 改用已在 OTA 中验证的 IDF esp_http_client。
//
// Lua 用法:
//   http.get("http://example.com/api")
//   local code, body = http.response()
//   http.close()

#include "starboard_lua.h"
#include <Arduino.h>
#include <esp_http_client.h>
#include <string>

// 每个请求创建一个新的 client,请求结束后销毁(简化实现,无全局状态)

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int lua_http_get(lua_State *L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "参数: url");
    const char *url = luaL_checkstring(L, 1);

    Serial.printf("[HTTP] GET %s\n", url);

    // HTTP 请求配置
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        lua_pushnil(L);
        lua_pushstring(L, "init failed");
        return 2;
    }

    esp_err_t err = esp_http_client_open(client, 0); // GET
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    int statusCode = esp_http_client_get_status_code(client);

    // 读取全部响应体(最大 16KB)
    char buf[256];
    std::string body;
    while (true)
    {
        int readLen = esp_http_client_read(client, buf, sizeof(buf));
        if (readLen <= 0) break;
        body.append(buf, readLen);
        if (body.size() > 16384) break; // 16KB 限
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    Serial.printf("[HTTP] 响应 code=%d body_len=%d\n", statusCode, (int)body.size());

    lua_pushinteger(L, statusCode);
    lua_pushstring(L, body.c_str());
    return 2;
}

static const luaL_Reg _lualib[] = {
    {"get", lua_http_get},
    {NULL, NULL},
};

extern "C" int luaopen_http(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}