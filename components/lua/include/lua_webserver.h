#ifndef LUA_WEBSERVER_H
#define LUA_WEBSERVER_H

/**
 * @brief 启动 Blockly Web 服务器(WiFi 已连接时调用)。
 * 提供 Blockly 可视化编辑器(CDN 加载) + API 保存/加载/运行 Lua App。
 */
void startBlocklyServer();

/**
 * @brief 处理 Web 客户端请求(应在主循环中间歇调用)。
 */
void handleBlocklyClient();

#endif // LUA_WEBSERVER_H
