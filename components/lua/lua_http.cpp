// lua_http —— Lua 绑定: HTTP 请求(IDF 原生 esp_http_client)
//
// 不用 Arduino HTTPClient(避免 NetworkClientSecure SSL 链接问题),
// 改用已在 OTA 中验证的 IDF esp_http_client。
//
// Lua 用法:
//   local code, body = http.get("http://example.com/api")           -- 第二参数可选:超时毫秒,默认3000
//   local code, body = http.get(url, 5000)
//   http.jsonGet(body, "hex")            -- 取字符串/标量字段（数组字段取第一个元素）
//   http.jsonArray(body, "explanation")  -- 取数组字段所有字符串元素 -> {"...", "..."}

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
    int n = lua_gettop(L);
    if (n < 1 || n > 2)
        return luaL_error(L, "参数: url [, timeout_ms]");
    const char *url = luaL_checkstring(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 3000);
    if (timeout_ms < 100) timeout_ms = 100; // 下限,避免过短

    Serial.printf("[HTTP] GET %s (timeout %dms)\n", url, timeout_ms);

    // HTTP 请求配置
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms; // 超时(默认3000):同步阻塞期间 hook 不触发,最坏延迟此值才响应停止

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
    if (*p == '[') {                                         // 数组值：取第一个字符串元素
        p++;                                                 // 跳过 '['
        while (p < end && *p != ']') {
            while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p==',')) p++;  // 跳过元素间分隔
            if (p >= end || *p == ']') break;
            if (*p == '"') {                                 // 第一个字符串元素
                p++;
                const char *s = p;
                while (p < end && *p != '"') p++;            // 读到闭合 "（不处理转义，与字符串分支一致）
                lua_pushlstring(L, s, p - s);
                return 1;
            }
            while (p < end && *p != ',' && *p != ']') p++;   // 非字符串元素，跳过
        }
        lua_pushliteral(L, "");                              // 空数组或无字符串元素 → 空串
        return 1;
    }
    if (*p == '"') {                                         // 字符串值："..."
        p++;
        const char *s = p;
        while (p < end && *p != '"') p++;
        lua_pushlstring(L, s, p - s);
    } else {                                                 // 数字/布尔/null：读到分隔符
        const char *s = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != ' ') p++;
        std::string tok(s, p - s);
        char *endp = nullptr;
        double d = strtod(tok.c_str(), &endp);
        if (!tok.empty() && endp == tok.c_str() + tok.size() &&
            tok.find_first_not_of("0123456789.+-eE") == std::string::npos)
            lua_pushnumber(L, d);                            // 数字字段 → number
        else
            lua_pushlstring(L, s, p - s);                    // true/false/null → 字符串
    }
    return 1;
}

// 从 JSON 提取一个数组字段的所有字符串元素，返回 Lua 表（整数索引 1..N）。
// 非数组字段或找不到时返回空表 {}。
// 例：http.jsonArray(body, "explanation") -> {"第一条释义", "第二条释义", ...}
static int http_jsonArray(lua_State *L)
{
    size_t jlen, klen;
    const char *json = luaL_checklstring(L, 1, &jlen);
    const char *key  = luaL_checklstring(L, 2, &klen);
    std::string needle = "\"" + std::string(key, klen) + "\"";
    const char *end = json + jlen;
    const char *p = std::search(json, end, needle.begin(), needle.end());

    lua_newtable(L);                              // 结果表（默认空）
    if (p == end) return 1;                        // 没找到 "key" → 空表
    p += needle.size();
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
    if (p >= end || *p != '[') return 1;           // 值不是数组 → 空表

    p++;                                          // 跳过 '['
    int idx = 1;                                  // Lua 表从 1 开始
    while (p < end && *p != ']') {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;  // 跳过元素间分隔
        if (p >= end || *p == ']') break;
        if (*p == '"') {                          // 字符串元素
            p++;
            const char *s = p;
            while (p < end && *p != '"') p++;     // 读到闭合 "（不处理转义）
            lua_pushlstring(L, s, p - s);
            lua_rawseti(L, -2, idx++);            // t[idx] = s, idx 自增
            if (p < end) p++;                     // 跳过闭合 "
        } else {                                  // 非字符串元素（数字/布尔/null），跳过不入表
            while (p < end && *p != ',' && *p != ']') p++;
        }
    }
    return 1;
}

static const luaL_Reg _lualib[] = {
    {"get", lua_http_get},
    {"jsonGet", http_jsonGet},
    {"jsonArray", http_jsonArray},
    {NULL, NULL},
};

extern "C" int luaopen_http(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}