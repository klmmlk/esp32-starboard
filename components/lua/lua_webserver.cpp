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
#include <dirent.h>
#include <starboard_hal.h>
#include "starboard_lua.h"

static WebServer server(80);
static bool serverStarted = false;
static volatile unsigned long lastClientActivityMs = 0; // 最后一次有客户端访问的时间
// 待运行的 App 名(由 /api/run 设置,主线程 pollRunRequest 检测并执行,避免在 webserver 线程刷屏)
static String pendingRunApp;
static volatile bool pendingRun = false;
// 执行互斥锁(全局,跨 pollRunRequest 调用有效)
static volatile bool isRunning = false;
// 每次 handler 被调用(即有客户端请求)时更新活动时间,用于空闲超时判断
static void updateActivity() { lastClientActivityMs = millis(); }

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
  <button onclick="flashCode()">&#128295; 烧录</button>
  <button onclick="runCode()">&#9654; 运行</button>
  <button onclick="deleteApp()">&#128465; 删除</button>
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
      {'kind': 'button', 'text': '创建变量', 'callbackKey': 'create_var'},
    ]},
    {'kind': 'category', 'name': '逻辑', 'colour': '#5b80a5', 'contents': [
      {'kind': 'block', 'type': 'controls_if'},
      {'kind': 'block', 'type': 'logic_compare'},
      {'kind': 'block', 'type': 'logic_operation'},
      {'kind': 'block', 'type': 'logic_negate'},
      {'kind': 'block', 'type': 'logic_boolean'},
      {'kind': 'block', 'type': 'logic_null'},
      {'kind': 'block', 'type': 'logic_ternary'},
    ]},
    {'kind': 'category', 'name': '循环', 'colour': '#5ba55b', 'contents': [
      {'kind': 'block', 'type': 'controls_repeat_ext'},
      {'kind': 'block', 'type': 'controls_whileUntil'},
      {'kind': 'block', 'type': 'controls_for'},
      {'kind': 'block', 'type': 'controls_forEach'},
      {'kind': 'block', 'type': 'controls_flow_statements'},
    ]},
    {'kind': 'category', 'name': '数学', 'colour': '#5b67a5', 'contents': [
      {'kind': 'block', 'type': 'math_number'},
      {'kind': 'block', 'type': 'math_arithmetic'},
      {'kind': 'block', 'type': 'math_single'},
      {'kind': 'block', 'type': 'math_trig'},
      {'kind': 'block', 'type': 'math_constant'},
      {'kind': 'block', 'type': 'math_number_property'},
      {'kind': 'block', 'type': 'math_round'},
      {'kind': 'block', 'type': 'math_modulo'},
      {'kind': 'block', 'type': 'math_constrain'},
      {'kind': 'block', 'type': 'math_random_int'},
      {'kind': 'block', 'type': 'math_random_float'},
    ]},
    {'kind': 'category', 'name': '文本', 'colour': '#5ba58c', 'contents': [
      {'kind': 'block', 'type': 'text'},
      {'kind': 'block', 'type': 'text_join'},
      {'kind': 'block', 'type': 'text_append'},
      {'kind': 'block', 'type': 'text_length'},
      {'kind': 'block', 'type': 'text_isEmpty'},
      {'kind': 'block', 'type': 'text_indexOf'},
      {'kind': 'block', 'type': 'text_charAt'},
      {'kind': 'block', 'type': 'text_getSubstring'},
      {'kind': 'block', 'type': 'text_changeCase'},
      {'kind': 'block', 'type': 'text_trim'},
      {'kind': 'block', 'type': 'text_print'},
    ]},
    {'kind': 'category', 'name': '列表', 'colour': '#745ba5', 'contents': [
      {'kind': 'block', 'type': 'lists_create_with'},
      {'kind': 'block', 'type': 'lists_create_with'},
      {'kind': 'block', 'type': 'lists_repeat'},
      {'kind': 'block', 'type': 'lists_length'},
      {'kind': 'block', 'type': 'lists_isEmpty'},
      {'kind': 'block', 'type': 'lists_indexOf'},
      {'kind': 'block', 'type': 'lists_getIndex'},
      {'kind': 'block', 'type': 'lists_setIndex'},
      {'kind': 'block', 'type': 'lists_getSublist'},
      {'kind': 'block', 'type': 'lists_split'},
      {'kind': 'block', 'type': 'lists_sort'},
    ]},
  ]
};

// Blockly 自定义块定义
Blockly.defineBlocksWithJsonArray([
  {"type":"display_beginframe","message0":"开始绘制帧","previousStatement":null,"colour":120},
  {"type":"display_endframe","message0":"结束绘制帧(刷屏)","previousStatement":null,"colour":120},
  {"type":"display_clearscreen","message0":"清屏 %1","args0":[{"type":"field_dropdown","name":"COLOR","options":[["白色","1"],["黑色","0"],["红色","63488"]]}],"previousStatement":null,"colour":120},
  {"type":"display_drawrect","message0":"画矩形 x:%1 y:%2 w:%3 h:%4 颜色:%5","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"W","value":100},{"type":"input_value","name":"H","value":100},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","63488"]]}],"inputsInline":true,"previousStatement":null,"colour":120},
  {"type":"display_fillcircle","message0":"填充圆 x:%1 y:%2 r:%3 颜色:%4","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"R","value":30},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","63488"]]}],"inputsInline":true,"previousStatement":null,"colour":120},
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

// --- 各积木的 Lua 代码生成器 ---
Blockly.Lua.forBlock['display_beginframe'] = function(b) { return 'display.beginFrame()\n'; };
Blockly.Lua.forBlock['display_endframe'] = function(b) { return 'display.endFrame()\n'; };
Blockly.Lua.forBlock['display_clearscreen'] = function(b) {
  return 'display.clearScreen(' + b.getFieldValue('COLOR') + ')\n';
};
Blockly.Lua.forBlock['display_drawrect'] = function(b) {
  var x = Blockly.Lua.valueToCode(b, 'X', 0) || '0';
  var y = Blockly.Lua.valueToCode(b, 'Y', 0) || '0';
  var w = Blockly.Lua.valueToCode(b, 'W', 0) || '100';
  var h = Blockly.Lua.valueToCode(b, 'H', 0) || '100';
  return 'display.drawRect(' + x + ',' + y + ',' + w + ',' + h + ',' + b.getFieldValue('COLOR') + ')\n';
};
Blockly.Lua.forBlock['display_fillcircle'] = function(b) {
  var x = Blockly.Lua.valueToCode(b, 'X', 0) || '0';
  var y = Blockly.Lua.valueToCode(b, 'Y', 0) || '0';
  var r = Blockly.Lua.valueToCode(b, 'R', 0) || '30';
  return 'display.fillCircle(' + x + ',' + y + ',' + r + ',' + b.getFieldValue('COLOR') + ')\n';
};
Blockly.Lua.forBlock['display_setcursor'] = function(b) {
  var x = Blockly.Lua.valueToCode(b, 'X', 0) || '0';
  var y = Blockly.Lua.valueToCode(b, 'Y', 0) || '0';
  return 'display.setCursor(' + x + ',' + y + ')\n';
};
Blockly.Lua.forBlock['display_print'] = function(b) {
  var t = Blockly.Lua.valueToCode(b, 'TEXT', 0) || '""';
  return 'display.u8g2Print(' + t + ')\n';
};
Blockly.Lua.forBlock['gui_msgbox'] = function(b) {
  var title = Blockly.Lua.valueToCode(b, 'TITLE', 0) || '""';
  var msg = Blockly.Lua.valueToCode(b, 'MSG', 0) || '""';
  return 'gui.msgbox(' + title + ',' + msg + ')\n';
};
Blockly.Lua.forBlock['gui_msgbox_yn'] = function(b) {
  var title = Blockly.Lua.valueToCode(b, 'TITLE', 0) || '""';
  var msg = Blockly.Lua.valueToCode(b, 'MSG', 0) || '""';
  return 'gui.msgbox_yn(' + title + ',' + msg + ')\n';
};
Blockly.Lua.forBlock['appmanager_gotoapp'] = function(b) {
  var n = Blockly.Lua.valueToCode(b, 'NAME', 0) || '""';
  return 'appManager.gotoApp(' + n + ')\n';
};
Blockly.Lua.forBlock['appmanager_goback'] = function(b) { return 'appManager.goBack()\n'; };
Blockly.Lua.forBlock['appmanager_setwakeupsec'] = function(b) {
  var s = Blockly.Lua.valueToCode(b, 'SEC', 0) || '60';
  return 'appManager.setWakeupSec(' + s + ')\n';
};
Blockly.Lua.forBlock['hal_gettime'] = function(b) { return 'hal.getTime()\n'; };
Blockly.Lua.forBlock['hal_timeinfo'] = function(b) {
  return ['hal.timeinfo()', 0];
};
Blockly.Lua.forBlock['http_get'] = function(b) {
  var u = Blockly.Lua.valueToCode(b, 'URL', 0) || '""';
  return ['http.get(' + u + ')', 0];
};

// --- 代码生成 ---
function generateCode() {
  let code = Blockly.Lua.workspaceToCode(workspace);
  document.getElementById('code').value = code;
}

// --- 保存(下载 XML 到用户电脑) ---
function saveCode() {
  let xml = Blockly.Xml.workspaceToDom(workspace);
  let xmlText = Blockly.Xml.domToText(xml);
  let appName = document.getElementById('appList').value || 'myapp';
  let blob = new Blob([xmlText], {type: 'application/xml'});
  let a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = appName + '.xml';
  a.click();
  setStatus('已下载: ' + appName + '.xml');
}

// --- 打开(上传 XML 还原积木) ---
function loadCode() {
  let input = document.createElement('input');
  input.type = 'file';
  input.accept = '.xml';
  input.onchange = function(e) {
    let file = e.target.files[0];
    if (!file) return;
    let reader = new FileReader();
    reader.onload = function(ev) {
      try {
        let xml = Blockly.utils.xml.textToDom(ev.target.result);
        workspace.clear();
        Blockly.Xml.domToWorkspace(xml, workspace);
        // 从文件名提取 App 名
        let name = file.name.replace(/\.xml$/i, '');
        document.getElementById('appList').value = name;
        setStatus('已加载: ' + name);
      } catch(err) {
        alert('XML 解析失败: ' + err.message);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}

// --- 删除 ---
function deleteApp() {
  let appName = document.getElementById('appList').value;
  if (!appName) { alert('请先选择要删除的 App'); return; }
  if (!confirm('确定删除 App "' + appName + '" 吗?此操作不可恢复。')) return;
  setStatus('删除中...');
  fetch('/api/delete?name=' + encodeURIComponent(appName))
    .then(r => r.text()).then(msg => {
      setStatus(msg);
      document.getElementById('appList').value = '';
      document.getElementById('code').value = '';
      refreshAppList();
    });
}

// --- 运行 ---
// --- 烧录(把 Lua 代码写到设备 LittleFS) ---
function flashCode() {
  let code = document.getElementById('code').value;
  // 自动包 beginFrame/endFrame
  if (code.indexOf('beginFrame') < 0) {
    code = 'display.beginFrame()\n' + code + '\ndisplay.endFrame()\n';
  }
  let appName = document.getElementById('appList').value;
  if (!appName) { appName = prompt('App 名称:'); if (!appName) return; }
  document.getElementById('appList').value = appName;
  setStatus('烧录中...');
  fetch('/api/save', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'name=' + encodeURIComponent(appName) + '&code=' + encodeURIComponent(code)
  }).then(r => r.text()).then(msg => {
    setStatus(msg);
    refreshAppList();
  });
}

// --- 运行(执行设备上已烧录的 App) ---
function runCode() {
  let appName = document.getElementById('appList').value;
  if (!appName) { alert('请先选择/烧录一个 App'); return; }
  setStatus('运行: ' + appName);
  fetch('/api/run?name=' + encodeURIComponent(appName));
}

function setStatus(msg) {
  document.getElementById('status').textContent = msg;
}

function onAppSelect() {
  // 设备上只存 Lua 代码,选 App 不自动加载积木(积木由用户上传 XML)
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
// "创建变量"按钮:弹框输入变量名,注册到工作区
workspace.registerButtonCallback('create_var', function(button) {
  let name = prompt('变量名(英文):');
  if (name) {
    workspace.createVariable(name);
  }
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
    updateActivity();
    server.send(200, "text/html", PAGE_BLOCKLY);
}

static void handleApiList()
{
    updateActivity();
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
    updateActivity();
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
    updateActivity();
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
    static String lastRequestId;
    static unsigned long lastRequestTimeMs = 0;

    // TCP 重传去重：如果请求内容和上次相同且间隔<10秒，认为是 TCP 重传
    // Lua App 执行约需 5 秒，需要足够的窗口来捕获重传
    String requestId = server.uri() + "?" + (server.hasArg("name") ? server.arg("name") : "");
    unsigned long now = millis();
    if (requestId == lastRequestId && (now - lastRequestTimeMs) < 10000)
    {
        server.send(200, "text/plain", "already queued");
        return;
    }

    // 如果正在执行 Lua App 或有待处理请求，拒绝
    if (isRunning || pendingRun)
    {
        server.send(429, "text/plain", "busy");
        return;
    }

    updateActivity();
    if (!server.hasArg("name"))
    {
        server.send(400, "text/plain", "missing name");
        return;
    }
    lastRequestId = requestId;
    lastRequestTimeMs = now;
    pendingRunApp = server.arg("name");
    pendingRun = true;
    Serial.printf("[Web] 运行 App: %s\n", pendingRunApp.c_str());
    server.send(200, "text/plain", "queued: " + pendingRunApp);
}

static void handleApiDelete()
{
    updateActivity();
    if (!server.hasArg("name"))
    {
        server.send(400, "text/plain", "missing name");
        return;
    }
    String name = server.arg("name");

    // 删除目录下所有文件(conf.lua/main.lua/icon 等),再删目录
    char dirPath[128];
    snprintf(dirPath, sizeof(dirPath), "/littlefs/apps/%s", name.c_str());

    DIR *dir = opendir(dirPath);
    if (dir)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char filePath[192];
            snprintf(filePath, sizeof(filePath), "%s/%s", dirPath, ent->d_name);
            unlink(filePath);
        }
        closedir(dir);
    }
    rmdir(dirPath);

    Serial.printf("[Web] 已删除 App: %s\n", name.c_str());
    server.send(200, "text/plain", "deleted: " + name);
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
    server.on("/api/delete", handleApiDelete);

    server.begin();
    lastClientActivityMs = millis();
    serverStarted = true;
    Serial.printf("[Web] Blockly 服务器已启动(http://%s/)\n", hal.wifiIp.c_str());
}

void handleBlocklyClient()
{
    if (!serverStarted)
        return;
    server.handleClient();
}

// 是否空闲超时(无客户端活动超过 idleSec 秒)
bool blocklyServerIdleTimeout(unsigned long idleSec)
{
    if (!serverStarted)
        return false;
    return (millis() - lastClientActivityMs) > (idleSec * 1000UL);
}

// 主线程调用:检测是否有待运行的 App,有则在主线程执行(避免 display 冲突)
// 返回 true 表示执行了一次(无论成功与否)
bool pollRunRequest()
{
    if (!serverStarted || !pendingRun)
        return false;

    // 执行互斥锁:防止重入
    if (isRunning)
        return false;
    isRunning = true;

    // 取走待处理的 App 名并清除标志
    String appName = pendingRunApp;
    pendingRun = false;

    char path[128];
    snprintf(path, sizeof(path), "/littlefs/apps/%s/main.lua", appName.c_str());

    lua_State *L = openLua();
    if (L)
    {
        Serial.printf("[Web] 运行 App: %s\n", appName.c_str());
        lua_execute(L, path);
        closeLua(L);
    }

    isRunning = false;
    return true;
}