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

/**
 * @brief 判断 Web 服务器是否空闲超时(无客户端活动超过 idleSec 秒)。
 *        有活跃请求时不超时;长时间无人访问才返回 true。
 */
bool blocklyServerIdleTimeout(unsigned long idleSec);

/**
 * @brief 主线程调用:检测是否有待运行的 App 请求(/api/run 设的标志),
 *        有则在当前主线程执行(避免和 display 刷新冲突)。返回 true 表示执行了一次。
 */
bool pollRunRequest();

#endif // LUA_WEBSERVER_H
