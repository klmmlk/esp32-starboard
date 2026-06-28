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
#include <algorithm>          // std::search（jsonGet 用）

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
    cfg.timeout_ms = 3000; // 收窄盲区:同步阻塞期间 hook 不触发,最坏延迟 3s 才响应停止

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
        Serial.printf("[HTTP] 连接失败: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    // 读取响应头（解析 status_code 与 content-length）；不调则 get_status_code 可能返回 0
    int contentLength = esp_http_client_fetch_headers(client);
    int statusCode = esp_http_client_get_status_code(client);
    Serial.printf("[HTTP] status=%d content_length=%d\n", statusCode, contentLength);

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

// 从 JSON 字符串提取一个字段的值（扁平字段：字符串或数字/布尔；不处理嵌套对象/转义）。
// 设备侧用途：http.get 返回的 JSON 里抠 hex/pinyin 等字段。找不到返回空串。
// 例：http.jsonGet(body, "hex") → "B400B400000..."
static int http_jsonGet(lua_State *L)
{
    size_t jlen, klen;
    const char *json = luaL_checklstring(L, 1, &jlen);
    const char *key  = luaL_checklstring(L, 2, &klen);
    std::string needle = "\"" + std::string(key, klen) + "\"";
    const char *end = json + jlen;
    const char *p = std::search(json, end, needle.begin(), needle.end());
    if (p == end) { lua_pushliteral(L, ""); return 1; }      // 没找到 "key"
    p += needle.size();
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
    if (p >= end) { lua_pushliteral(L, ""); return 1; }
    if (*p == '"') {                                         // 字符串值："..."
        p++;
        const char *s = p;
        while (p < end && *p != '"') p++;
        lua_pushlstring(L, s, p - s);
    } else {                                                 // 数字/布尔/null：读到分隔符
        const char *s = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != ' ') p++;
        lua_pushlstring(L, s, p - s);
    }
    return 1;
}

static const luaL_Reg _lualib[] = {
    {"get", lua_http_get},
    {"jsonGet", http_jsonGet},
    {NULL, NULL},
};

extern "C" int luaopen_http(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}