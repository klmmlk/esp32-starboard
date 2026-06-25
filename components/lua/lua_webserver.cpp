// lua_webserver —— Blockly 可视化编程 Web 服务器
//
// 用 Arduino WebServer(arduino-esp32 内置)提供:
//   /          - Blockly 可视化编辑器(CDN 加载)
//   /api/list  - 列出 /littlefs/apps/ 下的 Lua App
//   /api/save  - 保存 Lua 脚本到 /littlefs/apps/
//   /api/load  - 读取 Lua 脚本内容
//
// Blockly 核心库从 CDN 加载(不嵌入固件,省 2.6MB)。

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <dirent.h>
#include <starboard_hal.h>
#include "starboard_lua.h"

static WebServer server(80);
static bool serverStarted = false;

// HTML 页面:嵌入 Blockly + 定制工具箱
static const char PAGE_BLOCKLY[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Blockly - esp32-starboard</title>
<script src="https://unpkg.com/blockly/blockly.min.js"></script>
<script src="https://unpkg.com/blockly/blocks_compressed.js"></script>
<script src="https://unpkg.com/blockly/javascript_compressed.js"></script>
<script src="https://unpkg.com/blockly/lua_compressed.js"></script>
<link rel="stylesheet" href="https://unpkg.com/blockly/blockly.min.css">
<style>
  html, body { height: 100%; margin: 0; }
  #blocklyDiv { height: 70%; width: 100%; }
  #codeDiv { height: 30%; width: 100%; }
  #codeDiv textarea { width: 98%; height: 80%; margin: 1%; font-family: monospace; }
  #toolbar { padding: 8px; background: #eee; }
  #toolbar button { margin-right: 8px; padding: 6px 16px; }
  #appList { margin: 0 8px; padding: 4px; }
</style>
</head>
<body>
<div id="toolbar">
  <button onclick="saveCode()">&#128190; 保存</button>
  <button onclick="loadCode()">&#128194; 打开</button>
  <button onclick="runCode()">&#9654; 运行</button>
  <select id="appList" onchange="onAppSelect()">
    <option value="">-- 新建 App --</option>
  </select>
  <span id="status" style="margin-left:16px;color:#666;"></span>
</div>
<div id="blocklyDiv"></div>
<div id="codeDiv">
  <textarea id="code" placeholder="Lua code..."></textarea>
</div>
<script>
// 自定义工具箱(starboard API)
const TOOLBOX = {
  'kind': 'categoryToolbox',
  'contents': [
    {'kind': 'category', 'name': '显示', 'colour': '#4CAF50', 'contents': [
      {'kind': 'block', 'type': 'display_beginframe'},
      {'kind': 'block', 'type': 'display_endframe'},
      {'kind': 'block', 'type': 'display_clearscreen'},
      {'kind': 'block', 'type': 'display_drawrect'},
      {'kind': 'block', 'type': 'display_fillcircle'},
      {'kind': 'block', 'type': 'display_setcursor'},
      {'kind': 'block', 'type': 'display_print'},
    ]},
    {'kind': 'category', 'name': 'GUI', 'colour': '#2196F3', 'contents': [
      {'kind': 'block', 'type': 'gui_msgbox'},
      {'kind': 'block', 'type': 'gui_msgbox_yn'},
    ]},
    {'kind': 'category', 'name': 'App', 'colour': '#FF9800', 'contents': [
      {'kind': 'block', 'type': 'appmanager_gotoapp'},
      {'kind': 'block', 'type': 'appmanager_goback'},
      {'kind': 'block', 'type': 'appmanager_setwakeupsec'},
    ]},
    {'kind': 'category', 'name': '时间', 'colour': '#9C27B0', 'contents': [
      {'kind': 'block', 'type': 'hal_gettime'},
      {'kind': 'block', 'type': 'hal_timeinfo'},
    ]},
    {'kind': 'category', 'name': 'HTTP', 'colour': '#607D8B', 'contents': [
      {'kind': 'block', 'type': 'http_get'},
    ]},
    {'kind': 'category', 'name': '变量', 'colour': '#E91E63', 'contents': [
      {'kind': 'block', 'type': 'variables_get'},
      {'kind': 'block', 'type': 'variables_set'},
    ]},
    {'kind': 'category', 'name': '逻辑', 'colour': '#F44336', 'contents': [
      {'kind': 'block', 'type': 'controls_if'},
      {'kind': 'block', 'type': 'logic_compare'},
      {'kind': 'block', 'type': 'logic_operation'},
    ]},
    {'kind': 'category', 'name': '循环', 'colour': '#3F51B5', 'contents': [
      {'kind': 'block', 'type': 'controls_repeat_ext'},
      {'kind': 'block', 'type': 'controls_whileUntil'},
    ]},
  ]
};

// Blockly 自定义块定义
Blockly.defineBlocksWithJsonArray([
  {"type":"display_beginframe","message0":"开始绘制帧","previousStatement":null,"colour":120},
  {"type":"display_endframe","message0":"结束绘制帧(刷屏)","previousStatement":null,"colour":120},
  {"type":"display_clearscreen","message0":"清屏 %1","args0":[{"type":"field_dropdown","name":"COLOR","options":[["白色","1"],["黑色","0"],["红色","2"]]}],"previousStatement":null,"colour":120},
  {"type":"display_drawrect","message0":"画矩形 x:%1 y:%2 w:%3 h:%4 颜色:%5","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"W","value":100},{"type":"input_value","name":"H","value":100},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","2"]]}],"inputsInline":true,"previousStatement":null,"colour":120},
  {"type":"display_fillcircle","message0":"填充圆 x:%1 y:%2 r:%3 颜色:%4","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"R","value":30},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","2"]]}],"inputsInline":true,"previousStatement":null,"colour":120},
  {"type":"display_setcursor","message0":"设置光标 x:%1 y:%2","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"}],"inputsInline":true,"previousStatement":null,"colour":120},
  {"type":"display_print","message0":"显示文字 %1","args0":[{"type":"input_value","name":"TEXT"}],"previousStatement":null,"colour":120},
  {"type":"gui_msgbox","message0":"消息框 标题:%1 内容:%2","args0":[{"type":"input_value","name":"TITLE"},{"type":"input_value","name":"MSG"}],"inputsInline":true,"previousStatement":null,"colour":210},
  {"type":"gui_msgbox_yn","message0":"确认框 标题:%1 内容:%2","args0":[{"type":"input_value","name":"TITLE"},{"type":"input_value","name":"MSG"}],"inputsInline":true,"previousStatement":null,"colour":210},
  {"type":"appmanager_gotoapp","message0":"切换到App %1","args0":[{"type":"input_value","name":"NAME"}],"previousStatement":null,"colour":330},
  {"type":"appmanager_goback","message0":"返回上层App","previousStatement":null,"colour":330},
  {"type":"appmanager_setwakeupsec","message0":"设唤醒秒数 %1","args0":[{"type":"input_value","name":"SEC","value":60}],"previousStatement":null,"colour":330},
  {"type":"hal_gettime","message0":"更新时间","previousStatement":null,"colour":290},
  {"type":"hal_timeinfo","message0":"时间(year,month,day,hour,min,sec)","output":null,"colour":290},
  {"type":"http_get","message0":"HTTP GET %1","args0":[{"type":"input_value","name":"URL"}],"output":null,"colour":180},
]);

// --- 代码生成 ---
function generateCode() {
  let code = Blockly.Lua.workspaceToCode(workspace);
  document.getElementById('code').value = code;
}

// --- 保存 ---
function saveCode() {
  let code = document.getElementById('code').value;
  let appName = document.getElementById('appList').value;
  if (!appName) { appName = prompt('App 名称:'); if (!appName) return; }
  setStatus('保存中...');
  fetch('/api/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'name=' + encodeURIComponent(appName) + '&code=' + encodeURIComponent(code)
  }).then(r => r.text()).then(msg => {
    setStatus(msg);
    refreshAppList();
  });
}

// --- 打开 ---
function loadCode() {
  let appName = document.getElementById('appList').value;
  if (!appName) { alert('请先选择一个 App'); return; }
  fetch('/api/load?name=' + encodeURIComponent(appName))
    .then(r => r.text()).then(code => {
      document.getElementById('code').value = code;
      try {
        Blockly.Xml.domToWorkspace(Blockly.Xml.textToDom('<xml></xml>'), workspace);
        Blockly.Lua.workspaceToCode(workspace);
      } catch(e) {}
      setStatus('已加载: ' + appName);
    });
}

// --- 运行 ---
function runCode() {
  let appName = document.getElementById('appList').value;
  if (!appName) { alert('请先保存再运行'); return; }
  fetch('/api/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'name=' + encodeURIComponent(appName) + '&code=' + encodeURIComponent(document.getElementById('code').value)
  }).then(() => {
    fetch('/api/run?name=' + encodeURIComponent(appName));
    setStatus('运行: ' + appName);
  });
}

function setStatus(msg) {
  document.getElementById('status').textContent = msg;
}

function onAppSelect() {
  let v = document.getElementById('appList').value;
  if (v) loadCode();
}

function refreshAppList() {
  fetch('/api/list').then(r => r.json()).then(list => {
    let sel = document.getElementById('appList');
    let cur = sel.value;
    sel.innerHTML = '<option value="">-- 新建 App --</option>';
    list.forEach(n => {
      let opt = document.createElement('option');
      opt.value = n; opt.textContent = n;
      if (n == cur) opt.selected = true;
      sel.appendChild(opt);
    });
  });
}

// --- 初始化 ---
let workspace = Blockly.inject('blocklyDiv', {
  toolbox: TOOLBOX,
  media: 'https://unpkg.com/blockly/media/',
  zoom: {controls: true, wheel: true}
});
workspace.addChangeListener(generateCode);
refreshAppList();
generateCode();
</script>
</body>
</html>
)=====";

// ---------- HTTP 路由 ----------

static void handleRoot()
{
    server.send(200, "text/html", PAGE_BLOCKLY);
}

static void handleApiList()
{
    // 扫描 /littlefs/apps/ 返回 JSON 数组
    String json = "[";
    bool first = true;
    DIR *dir = opendir("/littlefs/apps");
    if (dir)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (ent->d_type != DT_DIR)
                continue;
            if (!first) json += ",";
            json += "\"";
            json += ent->d_name;
            json += "\"";
            first = false;
        }
        closedir(dir);
    }
    json += "]";
    server.send(200, "application/json", json);
}

static void handleApiLoad()
{
    if (!server.hasArg("name"))
    {
        server.send(400, "text/plain", "missing name");
        return;
    }
    String name = server.arg("name");
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/apps/%s/main.lua", name.c_str());

    FILE *f = fopen(path, "r");
    if (!f)
    {
        server.send(404, "text/plain", "not found");
        return;
    }

    String content;
    char buf[256];
    while (fgets(buf, sizeof(buf), f))
        content += buf;
    fclose(f);
    server.send(200, "text/plain; charset=utf-8", content);
}

static void handleApiSave()
{
    if (!server.hasArg("name") || !server.hasArg("code"))
    {
        server.send(400, "text/plain", "missing name or code");
        return;
    }
    String name = server.arg("name");
    String code = server.arg("code");

    // 创建目录
    char dirPath[128];
    snprintf(dirPath, sizeof(dirPath), "/littlefs/apps/%s", name.c_str());
    mkdir(dirPath, 0755);

    // 写 conf.lua
    char confPath[128];
    snprintf(confPath, sizeof(confPath), "/littlefs/apps/%s/conf.lua", name.c_str());
    FILE *f = fopen(confPath, "w");
    if (f) { fprintf(f, "title = \"%s\"\n", name.c_str()); fclose(f); }

    // 写 main.lua
    char luaPath[128];
    snprintf(luaPath, sizeof(luaPath), "/littlefs/apps/%s/main.lua", name.c_str());
    f = fopen(luaPath, "w");
    if (!f)
    {
        server.send(500, "text/plain", "write failed");
        return;
    }
    fprintf(f, "%s", code.c_str());
    fclose(f);

    Serial.printf("[Web] 已保存 App: %s (%u bytes)\n", name.c_str(), code.length());
    server.send(200, "text/plain", "OK: " + name);
}

static void handleApiRun()
{
    if (!server.hasArg("name"))
    {
        server.send(400, "text/plain", "missing name");
        return;
    }
    String name = server.arg("name");
    char path[128];
    snprintf(path, sizeof(path), "/littlefs/apps/%s/main.lua", name.c_str());

    // 简单的后台执行:打开 Lua,执行文件
    lua_State *L = openLua();
    if (L)
    {
        int ret = lua_execute(L, path);
        closeLua(L);
        if (ret == 0)
            server.send(200, "text/plain", "run ok");
        else
            server.send(500, "text/plain", "run failed");
    }
    else
    {
        server.send(500, "text/plain", "lua init failed");
    }
}

// ---------- 公共接口 ----------

void startBlocklyServer()
{
    if (serverStarted)
        return;

    server.on("/", handleRoot);
    server.on("/api/list", handleApiList);
    server.on("/api/load", handleApiLoad);
    server.on("/api/save", HTTP_POST, handleApiSave);
    server.on("/api/run", handleApiRun);

    server.begin();
    serverStarted = true;
    Serial.printf("[Web] Blockly 服务器已启动(http://%s/)\n", WiFi.localIP().toString().c_str());
}

void handleBlocklyClient()
{
    if (serverStarted)
        server.handleClient();
}