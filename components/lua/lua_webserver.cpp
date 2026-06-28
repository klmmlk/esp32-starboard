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
#include <starboard_config.h>   // PIN_BUTTONC
#include "starboard_lua.h"

static WebServer server(80);
static bool serverStarted = false;
static volatile unsigned long lastClientActivityMs = 0; // 最后一次有客户端访问的时间
// 待运行的 App 名(由 /api/run 设置,主线程 pollRunRequest 检测并执行,避免在 webserver 线程刷屏)
static String pendingRunApp;
static volatile bool pendingRun = false;
// 执行互斥锁(全局,跨 pollRunRequest 调用有效)
static volatile bool isRunning = false;
// Web IDE 保存/删除 App 后置位,主线程(appWebIDE 循环)检测并调 syncLuaApps 增量同步
static volatile bool g_appsDirty = false;
// 每次 handler 被调用(即有客户端请求)时更新活动时间,用于空闲超时判断
static void updateActivity() { lastClientActivityMs = millis(); }

// (强制停止监控任务 killMonitorTask 已移除:Lua 停止由 starboard_lua 的 LINE hook
//  统一接管 —— 中键长按>1s / 无操作超时,见 luaSysTick。无需独立监控任务)

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
  #toolbar { padding: 8px; background: #eee; position: sticky; top: 0; z-index: 100; border-bottom: 1px solid #ccc; }
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
  <button onclick="openImgDialogForSelected()" style="background:#9C27B0;color:white;border:none;">📷 上传图片</button>
  <select id="appList" onchange="onAppSelect()">
    <option value="">-- 新建 App --</option>
  </select>
  <span id="status" style="margin-left:16px;color:#666;"></span>
</div>
<!-- 图片上传模态对话框 -->
<div id="imgDialog" style="display:none;position:fixed;z-index:9999;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.5);">
  <div style="background:white;margin:60px auto;padding:20px;width:520px;border-radius:8px;text-align:center;">
    <h3 style="margin:0 0 12px;">上传图片数据</h3>
    <canvas id="imgPreview" style="max-width:400px;max-height:200px;border:1px solid #ccc;background:#f9f9f9;"></canvas>
    <div style="margin-top:10px;text-align:left;">
      <input type="file" id="imgFileInput" accept="image/*" style="display:none;">
      <button onclick="document.getElementById('imgFileInput').click()">选择图片</button>
      &nbsp;
      模式:
      <select id="imgMode" onchange="onImgModeChange()">
        <option value="bw">黑白图片</option>
        <option value="3color">三色图片(黑白+红色层)</option>
      </select>
    </div>
    <div style="margin-top:10px;">
      宽度 <input id="imgW" type="number" min="1" style="width:60px;"> px
      &nbsp;高度 <input id="imgH" type="number" min="1" style="width:60px;"> px
      &nbsp;<label><input id="imgLock" type="checkbox" checked> 锁定比例</label>
    </div>
    <div style="margin-top:10px;font-size:12px;color:#888;">
      当前编辑积木: <span id="imgTargetInfo">未选择</span>
      &nbsp;(请先在Blockly中选中一个<b>图片数据</b>积木,再点击上传)
    </div>
    <div style="margin-top:14px;">
      <button onclick="closeImgDialog()">取消</button>
      <button onclick="confirmImgDialog()" style="background:#4CAF50;color:white;border:none;padding:6px 20px;">确认并填入积木</button>
    </div>
  </div>
</div>
<div id="blocklyDiv"></div>
<div id="codeDiv">
  <textarea id="code" placeholder="Lua code..."></textarea>
</div>
<script>
// 同步获取 Lua App 列表(/api/list),用于"切换到App"积木下拉
var _appOpts = [['(无App)','']];   // 占位:设备上没有 Lua App 时显示
try {
  var _xhr = new XMLHttpRequest();
  _xhr.open('GET', '/api/list', false);   // 同步
  _xhr.send();
  if (_xhr.status == 200) {
    var list = JSON.parse(_xhr.responseText);
    if (list && list.length) {
      _appOpts = list.map(function(n){ return [n, n]; });
    }
  }
} catch(e) {}
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
    {'kind': 'category', 'name': '图片', 'colour': '#9C27B0', 'contents': [
      {'kind': 'block', 'type': 'image_bw'},
      {'kind': 'block', 'type': 'image_3color'},
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
      {'kind': 'block', 'type': 'hal_timefield'},
      {'kind': 'block', 'type': 'common_delay'},
      {'kind': 'block', 'type': 'hal_millis'},
    ]},
    {'kind': 'category', 'name': '按键', 'colour': '#FF5722', 'contents': [
      {'kind': 'block', 'type': 'gui_waitkey'},
      {'kind': 'block', 'type': 'gui_waitlongpress'},
      {'kind': 'block', 'type': 'gui_trygetkey'},
    ]},
    {'kind': 'category', 'name': '系统', 'colour': '#607D8B', 'contents': [
      {'kind': 'block', 'type': 'sys_yield'},
    ]},
    {'kind': 'category', 'name': 'HTTP', 'colour': '#607D8B', 'contents': [
      {'kind': 'block', 'type': 'http_get'},
    ]},
    {'kind': 'category', 'name': '数据', 'colour': '#0097A7', 'contents': [
      {'kind': 'block', 'type': 'data_save'},
      {'kind': 'block', 'type': 'data_load'},
    ]},
    {'kind': 'category', 'name': '变量', 'colour': '#E91E63', 'contents': [
      {'kind': 'block', 'type': 'variables_get'},
      {'kind': 'block', 'type': 'variables_set'},
    ]},
    {'kind': 'category', 'name': '函数', 'custom': 'PROCEDURE', 'colour': '#995ba5'},
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
  {"type":"display_beginframe","message0":"开始绘制帧","previousStatement":null,"nextStatement":null,"colour":120},
  {"type":"display_endframe","message0":"结束绘制帧(刷屏)","previousStatement":null,"nextStatement":null,"colour":120},
  {"type":"display_clearscreen","message0":"清屏 %1","args0":[{"type":"field_dropdown","name":"COLOR","options":[["白色","1"],["黑色","0"],["红色","63488"]]}],"previousStatement":null,"nextStatement":null,"colour":120},
  {"type":"display_drawrect","message0":"画矩形 x:%1 y:%2 w:%3 h:%4 颜色:%5","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"W","value":100},{"type":"input_value","name":"H","value":100},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","63488"]]}],"inputsInline":true,"previousStatement":null,"nextStatement":null,"colour":120},
  {"type":"display_fillcircle","message0":"填充圆 x:%1 y:%2 r:%3 颜色:%4","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"},{"type":"input_value","name":"R","value":30},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","63488"]]}],"inputsInline":true,"previousStatement":null,"nextStatement":null,"colour":120},
  {"type":"display_setcursor","message0":"设置光标 x:%1 y:%2","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"}],"inputsInline":true,"previousStatement":null,"nextStatement":"statement","colour":120},
  {"type":"display_print","message0":"显示文字 %1 字体 %2 颜色 %3","args0":[{"type":"input_value","name":"TEXT"},{"type":"field_dropdown","name":"FONT","options":[
    ["中文 16px","wqy16"],["中文 12px","wqy12"],
    ["英文 4x6","4x6"],["英文 5x7","5x7"],["英文 5x8","5x8"],
    ["英文 6x10","6x10"],["英文 6x12","6x12"],["英文 6x13","6x13"],
    ["英文 7x13","7x13"],["英文 7x14","7x14"],
    ["英文 8x13","8x13"],["英文 9x15","9x15"],["英文 9x18","9x18"],["英文 10x20","10x20"],
    ["粗体 6x13","bold6x13"],["粗体 7x13","bold7x13"],["粗体 8x13","bold8x13"],["粗体 9x15","bold9x15"],
    ["窄体 6x12","narrow6x12"],["窄体 6x13","narrow6x13"],["窄体 7x13","narrow7x13"],["窄体 8x13","narrow8x13"],
  ]},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"],["红色","63488"]]}],"previousStatement":"statement","nextStatement":"statement","colour":120},
  // --- 图片积木(上传图片:先选中积木,再点工具栏"📷 上传图片"按钮) ---
  {"type":"image_bw","message0":"显示黑白图片 x:%1 y:%2","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"}],"message1":"宽:%1 高:%2 颜色:%3","args1":[{"type":"input_value","name":"W"},{"type":"input_value","name":"H"},{"type":"field_dropdown","name":"COLOR","options":[["黑色","0"],["白色","1"]]}],"message2":"黑白数据: %1","args2":[{"type":"input_value","name":"HEX"}],"previousStatement":null,"nextStatement":null,"colour":280,"inputsInline":false},
  {"type":"image_3color","message0":"显示三色图片 x:%1 y:%2","args0":[{"type":"input_value","name":"X"},{"type":"input_value","name":"Y"}],"message1":"宽:%1 高:%2","args1":[{"type":"input_value","name":"W"},{"type":"input_value","name":"H"}],"message2":"黑白数据: %1","args2":[{"type":"input_value","name":"HEX_BW"}],"message3":"红色数据: %1","args3":[{"type":"input_value","name":"HEX_RED"}],"previousStatement":null,"nextStatement":null,"colour":280,"inputsInline":false},
  {"type":"gui_msgbox","message0":"消息框 标题:%1 内容:%2","args0":[{"type":"input_value","name":"TITLE"},{"type":"input_value","name":"MSG"}],"inputsInline":true,"previousStatement":null,"colour":210},
  {"type":"gui_msgbox_yn","message0":"确认框 标题:%1 内容:%2","args0":[{"type":"input_value","name":"TITLE"},{"type":"input_value","name":"MSG"}],"inputsInline":true,"previousStatement":null,"colour":210},
  {"type":"appmanager_goback","message0":"返回上层App","previousStatement":null,"colour":330},
  {"type":"appmanager_setwakeupsec","message0":"设唤醒秒数 %1","args0":[{"type":"input_value","name":"SEC","value":60}],"previousStatement":null,"colour":330},
  {"type":"hal_timefield","message0":"获取时间 %1","args0":[{"type":"field_dropdown","name":"F","options":[["年","year"],["月","month"],["日","day"],["时","hour"],["分","min"],["秒","sec"],["星期","wday"]]}],"output":null,"colour":290,"tooltip":"读取指定时间字段(自动刷新)"},
  {"type":"common_delay","message0":"延时(毫秒) %1","args0":[{"type":"input_value","name":"MS"}],"previousStatement":null,"nextStatement":null,"colour":290},
  {"type":"hal_millis","message0":"开机毫秒数","output":null,"colour":290,"tooltip":"用于空闲超时判断"},
  {"type":"gui_waitkey","message0":"等待按键","output":null,"colour":20,"tooltip":"阻塞等待任意按键被按下,返回 1=左键 2=中键 3=右键"},
  {"type":"gui_waitlongpress","message0":"等待按键 %1 被按下","args0":[{"type":"field_dropdown","name":"BTN","options":[["左键","1"],["中键","2"],["右键","3"]]}],"previousStatement":null,"nextStatement":null,"colour":20,"tooltip":"阻塞,直到指定按键被按下(忽略其他键)"},
  {"type":"gui_trygetkey","message0":"读取按键(无则返回0)","output":null,"colour":20,"tooltip":"非阻塞:有键返回1=左/2=中/3=右,无键返回0"},
  {"type":"sys_yield","message0":"让出CPU(放权)","previousStatement":null,"nextStatement":null,"colour":100,"tooltip":"在循环里定期调用,让系统检测睡眠/超时"},
  {"type":"http_get","message0":"HTTP GET %1","args0":[{"type":"input_value","name":"URL"}],"output":null,"colour":180},
  {"type":"data_save","message0":"保存数据 %1 为 %2","args0":[{"type":"input_value","name":"KEY"},{"type":"input_value","name":"VAL"}],"inputsInline":true,"previousStatement":null,"nextStatement":null,"colour":160,"tooltip":"持久化保存(重启不丢,按App隔离)"},
  {"type":"data_load","message0":"读取数据 %1 默认 %2","args0":[{"type":"input_value","name":"KEY"},{"type":"input_value","name":"DEF"}],"inputsInline":true,"output":null,"colour":160,"tooltip":"读取持久化数据,无则返回默认值"},
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
  var font = b.getFieldValue('FONT');
  var color = b.getFieldValue('COLOR');
  return 'display.setFont("' + font + '")\ndisplay.setTextColor(' + color + ')\ndisplay.u8g2Print(' + t + ')\n';
};
Blockly.Lua.forBlock['image_bw'] = function(b) {
  var x = Blockly.Lua.valueToCode(b, 'X', 0) || '0';
  var y = Blockly.Lua.valueToCode(b, 'Y', 0) || '0';
  var w = Blockly.Lua.valueToCode(b, 'W', 0) || '0';
  var h = Blockly.Lua.valueToCode(b, 'H', 0) || '0';
  var color = b.getFieldValue('COLOR');
  var hex = Blockly.Lua.valueToCode(b, 'HEX', 0) || '""';
  return 'gui.drawBWBM(' + x + ',' + y + ',' + w + ',' + h + ',' + hex + ',' + color + ')\n';
};
Blockly.Lua.forBlock['image_3color'] = function(b) {
  var x = Blockly.Lua.valueToCode(b, 'X', 0) || '0';
  var y = Blockly.Lua.valueToCode(b, 'Y', 0) || '0';
  var w = Blockly.Lua.valueToCode(b, 'W', 0) || '0';
  var h = Blockly.Lua.valueToCode(b, 'H', 0) || '0';
  var hexBw = Blockly.Lua.valueToCode(b, 'HEX_BW', 0) || '""';
  var hexRed = Blockly.Lua.valueToCode(b, 'HEX_RED', 0) || '""';
  return 'gui.draw3ColorBM(' + x + ',' + y + ',' + w + ',' + h + ',' + hexBw + ',' + hexRed + ')\n';
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
// 切换到App 积木:下拉用函数式 menuGenerator,每次点开都读最新 _appOpts
// (新保存的 App 经 refreshAppList 更新 _appOpts 后,无需刷新页面即可在下拉看到)
Blockly.Blocks['appmanager_gotoapp'] = {
  init: function() {
    this.appendDummyInput()
        .appendField('切换到App')
        .appendField(new Blockly.FieldDropdown(function(){ return _appOpts; }), 'NAME');
    this.setPreviousStatement(true, null);
    this.setColour(330);
  }
};
Blockly.Lua.forBlock['appmanager_gotoapp'] = function(b) {
  var n = b.getFieldValue('NAME');
  return 'appManager.gotoApp("' + n + '")\n';
};
Blockly.Lua.forBlock['appmanager_goback'] = function(b) { return 'appManager.goBack()\n'; };
Blockly.Lua.forBlock['appmanager_setwakeupsec'] = function(b) {
  var s = Blockly.Lua.valueToCode(b, 'SEC', 0) || '60';
  return 'appManager.setWakeupSec(' + s + ')\n';
};
Blockly.Lua.forBlock['hal_timefield'] = function(b) {
  var f = b.getFieldValue('F');
  return ['hal.timeField("' + f + '")', 0];
};
Blockly.Lua.forBlock['hal_millis'] = function(b) {
  return ['hal.millis()', 0];
};
Blockly.Lua.forBlock['common_delay'] = function(b) {
  var ms = Blockly.Lua.valueToCode(b, 'MS', 0) || '1000';
  return 'delay(' + ms + ')\n';
};
Blockly.Lua.forBlock['gui_waitlongpress'] = function(b) {
  var btn = b.getFieldValue('BTN');
  return 'gui.waitButton(' + btn + ')\n';
};
Blockly.Lua.forBlock['gui_trygetkey'] = function(b) {
  return ['gui.tryGetKey()', 0];
};
Blockly.Lua.forBlock['sys_yield'] = function(b) {
  return 'sys.yield()\n';
};
Blockly.Lua.forBlock['gui_waitkey'] = function(b) {
  return ['gui.waitKey()', 0];
};
Blockly.Lua.forBlock['http_get'] = function(b) {
  var u = Blockly.Lua.valueToCode(b, 'URL', 0) || '""';
  return ['http.get(' + u + ')', 0];
};
Blockly.Lua.forBlock['data_save'] = function(b) {
  var k = Blockly.Lua.valueToCode(b, 'KEY', 0) || '""';
  var v = Blockly.Lua.valueToCode(b, 'VAL', 0) || '0';
  return 'data.save(' + k + ',' + v + ')\n';
};
Blockly.Lua.forBlock['data_load'] = function(b) {
  var k = Blockly.Lua.valueToCode(b, 'KEY', 0) || '""';
  var d = Blockly.Lua.valueToCode(b, 'DEF', 0) || '0';
  return ['data.load(' + k + ',' + d + ')', 0];
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
  if (!appName) {
    let appName = prompt('输入 App 内部名称(英文/拼音,作为目录名):');
    if (!appName) return;
    let appTitle = prompt('输入 App 显示名称(中文,列表中可见):', appName);
    if (!appTitle) return;
    document.getElementById('appList').value = appName;
    setStatus('烧录中...');
    fetch('/api/save', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'name=' + encodeURIComponent(appName) + '&title=' + encodeURIComponent(appTitle) + '&code=' + encodeURIComponent(code)
    }).then(r => r.text()).then(msg => {
      setStatus(msg);
      refreshAppList();
    });
    return;
  }
  // 已选中的 App:直接烧录(用已保存的 title)
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
    // 同步更新"切换到App"积木下拉的数据源(下次点开积木下拉即生效)
    _appOpts = (list && list.length)
      ? list.map(function(n){ return [n, n]; })
      : [['(无App)','']];
  });
}

// --- 初始化 ---
let workspace = Blockly.inject('blocklyDiv', {
  toolbox: TOOLBOX,
  media: 'https://unpkg.com/blockly/media/',
  zoom: {controls: true, wheel: true}
});
// ========== 图片上传 ==========
// 当前选中的图片积木(通过Blockly事件追踪)
let _origImg = null, _origW = 0, _origH = 0;

// 工具栏"上传图片"按钮:弹出上传对话框
function openImgDialogForSelected() {
  // 查找工作区里的图片积木
  let imgBlocks = [];
  let all = workspace.getAllBlocks();
  for (let b of all) {
    if (b.type === 'image_bw' || b.type === 'image_3color') imgBlocks.push(b);
  }
  if (imgBlocks.length === 0) { alert('请先在工作区放置一个「显示黑白图片」或「显示三色图片」积木'); return; }
  // 只有一个时直接用,多个时让用户选
  if (imgBlocks.length > 1) {
    let names = imgBlocks.map((b,i) => i+1 + ': ' + b.type).join('\n');
    let sel = prompt('找到多个图片积木,请输入序号使用哪个(1-' + imgBlocks.length + '):\n' + names + '\n或者直接在工作区删除多余的,只留一个');
    if (!sel) return;
    let idx = parseInt(sel) - 1;
    if (idx < 0 || idx >= imgBlocks.length) { alert('无效选择'); return; }
    _imgUploadBlock = imgBlocks[idx];
  } else {
    _imgUploadBlock = imgBlocks[0];
  }
  let type = _imgUploadBlock.type;
  let mode = document.getElementById('imgMode').value;
  if (type === 'image_bw') {
    _imgUploadField = 'HEX';
  } else {
    _imgUploadField = (mode === '3color') ? 'HEX_RED' : 'HEX_BW';
  }
  document.getElementById('imgTargetInfo').textContent = type + ' / ' + _imgUploadField;
  document.getElementById('imgW').value = '';
  document.getElementById('imgH').value = '';
  document.getElementById('imgLock').checked = true;
  _origImg = null;
  let cv = document.getElementById('imgPreview');
  cv.width = 400; cv.height = 200;
  cv.getContext('2d').clearRect(0, 0, 400, 200);
  document.getElementById('imgDialog').style.display = 'block';
}

function onImgModeChange() {
  if (!_imgUploadBlock) return;
  let mode = document.getElementById('imgMode').value;
  _imgUploadField = (_imgUploadBlock.type === 'image_bw') ? 'HEX' :
                   (mode === '3color') ? 'HEX_RED' : 'HEX_BW';
  document.getElementById('imgTargetInfo').textContent = _imgUploadBlock.type + ' / ' + _imgUploadField;
}

function closeImgDialog() {
  document.getElementById('imgDialog').style.display = 'none';
  _imgUploadBlock = null;
}

function previewScaled() {
  if (!_origImg) return;
  let w = parseInt(document.getElementById('imgW').value) || 0;
  let h = parseInt(document.getElementById('imgH').value) || 0;
  if (w <= 0 || h <= 0) return;
  let cv = document.getElementById('imgPreview');
  cv.width = w; cv.height = h;
  cv.getContext('2d').drawImage(_origImg, 0, 0, w, h);
}

// 像素颜色判断:R 主导且足够鲜艳才算红(避免白/灰/黑被 HSV 色相0误判为红)
function _isRed(r, g, b) {
  return (r > 120 && (r - g) > 40 && (r - b) > 40);
}

function toLbmHex(canvas, w, h, mode) {
  let ctx = canvas.getContext('2d');
  let imgData = ctx.getImageData(0, 0, w, h).data;
  let rowBytes = Math.ceil(w / 8);
  let hexRows = [];
  for (let y = 0; y < h; y++) {
    let rowBytesArr = [];
    for (let byteIdx = 0; byteIdx < rowBytes; byteIdx++) {
      let byte = 0;
      for (let bit = 0; bit < 8; bit++) {
        let px = byteIdx * 8 + bit;
        if (px >= w) continue;
        let idx = (y * w + px) * 4;
        let r = imgData[idx], g = imgData[idx + 1], b = imgData[idx + 2];
        let gray = r * 0.299 + g * 0.587 + b * 0.114;
        let bitVal = 0;
        if (mode === 'bw') {
          // 黑白图:暗像素=1(红也当黑,纯黑白屏语义)
          bitVal = (gray < 128) ? 1 : 0;
        } else if (mode === 'black') {
          // 三色图黑白层:暗且非红=1(红色留给红色层)
          bitVal = (gray < 128 && !_isRed(r, g, b)) ? 1 : 0;
        } else { // 'red' 红色层
          bitVal = _isRed(r, g, b) ? 1 : 0;
        }
        byte |= (bitVal << bit);
      }
      rowBytesArr.push(byte);
    }
    hexRows.push(rowBytesArr.map(function(b){return b.toString(16).padStart(2,'0').toUpperCase();}).join(''));
  }
  let header = (w & 0xFF).toString(16).padStart(2,'0') + ((w >> 8) & 0xFF).toString(16).padStart(2,'0') +
               (h & 0xFF).toString(16).padStart(2,'0') + ((h >> 8) & 0xFF).toString(16).padStart(2,'0');
  return header + hexRows.join('');
}

// 把hex填入积木的指定字段(创建文本块并连接)
function _fillImgField(fieldName, hexStr) {
  if (!hexStr) return;
  let textBlock = workspace.newBlock('text');
  textBlock.initSvg();
  textBlock.render();
  textBlock.setFieldValue(hexStr, 'TEXT');
  let input = _imgUploadBlock.getInput(fieldName);
  if (input && input.connection) {
    input.connection.connect(textBlock.outputConnection);
  }
}

function confirmImgDialog() {
  if (!_imgUploadBlock) { alert('未选中积木'); return; }
  let w = parseInt(document.getElementById('imgW').value) || 0;
  let h = parseInt(document.getElementById('imgH').value) || 0;
  if (w <= 0 || h <= 0) { alert('请输入有效尺寸'); return; }
  let mode = document.getElementById('imgMode').value;
  let cv = document.getElementById('imgPreview');
  let type = _imgUploadBlock.type;
  if (type === 'image_3color') {
    // 三色图:一张图同时生成黑白层(黑色像素)和红色层(红色像素)
    _fillImgField('HEX_BW', toLbmHex(cv, w, h, 'black'));
    _fillImgField('HEX_RED', toLbmHex(cv, w, h, 'red'));
  } else {
    // 黑白图:单层(红当黑)
    _fillImgField('HEX', toLbmHex(cv, w, h, 'bw'));
  }
  closeImgDialog();
  setStatus('图片已填入 ' + w + 'x' + h + 'px' + (type === 'image_3color' ? '(黑白+红色两层)' : ''));
}

document.getElementById('imgFileInput').onchange = function(e) {
  let file = e.target.files[0];
  if (!file) return;
  let reader = new FileReader();
  reader.onload = function(ev) {
    let img = new Image();
    img.onload = function() {
      _origImg = img;
      _origW = img.width; _origH = img.height;
      let scale = Math.min(400 / _origW, 200 / _origH, 1);
      let w = Math.round(_origW * scale);
      let h = Math.round(_origH * scale);
      document.getElementById('imgW').value = w;
      document.getElementById('imgH').value = h;
      previewScaled();
    };
    img.src = ev.target.result;
  };
  reader.readAsDataURL(file);
  this.value = '';
};

document.getElementById('imgW').oninput = function() {
  let lock = document.getElementById('imgLock').checked;
  let w = parseInt(this.value) || 0;
  if (lock && w > 0 && _origW > 0) {
    let h = Math.round(w * _origH / _origW);
    document.getElementById('imgH').value = h;
  }
  previewScaled();
};
document.getElementById('imgH').oninput = function() {
  let lock = document.getElementById('imgLock').checked;
  let h = parseInt(this.value) || 0;
  if (lock && h > 0 && _origH > 0) {
    let w = Math.round(h * _origW / _origH);
    document.getElementById('imgW').value = w;
  }
  previewScaled();
};
// ---- 中键(滚轮按下)拖动平移画布;阻止 Blockly 把中键当积木操作 ----
let _panning = false, _panX = 0, _panY = 0;
document.addEventListener('mousedown', function(e) {
  if (e.button !== 1) return;            // 只处理中键
  if (!e.target.closest('#blocklyDiv')) return;  // 仅画布区域
  _panning = true;
  _panX = e.clientX; _panY = e.clientY;
  e.preventDefault();                    // 阻止浏览器自动滚动
  e.stopPropagation();                   // 阻止 Blockly 拖积木
}, true);                                // 捕获阶段,抢在 Blockly 之前
window.addEventListener('mousemove', function(e) {
  if (!_panning) return;
  let dx = e.clientX - _panX, dy = e.clientY - _panY;
  // Blockly scroll:鼠标右移看右侧内容 → scrollX 增大
  workspace.scroll(workspace.scrollX + dx, workspace.scrollY + dy);
  _panX = e.clientX; _panY = e.clientY;
});
window.addEventListener('mouseup', function(e) {
  if (e.button === 1) { _panning = false; e.preventDefault(); }
});
// 屏蔽中键的 auxclick(某些浏览器的自动滚动光标)
document.getElementById('blocklyDiv').addEventListener('auxclick', function(e) {
  if (e.button === 1) e.preventDefault();
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
    // title 可选,未提供时用 name 兜底
    String title = server.hasArg("title") ? server.arg("title") : name;

    // 创建目录
    char dirPath[128];
    snprintf(dirPath, sizeof(dirPath), "/littlefs/apps/%s", name.c_str());
    mkdir(dirPath, 0755);

    // 写 conf.lua: title 供 LuaAppWrapper 读取显示名
    char confPath[128];
    snprintf(confPath, sizeof(confPath), "/littlefs/apps/%s/conf.lua", name.c_str());
    FILE *f = fopen(confPath, "w");
    if (f) { fprintf(f, "title = \"%s\"\n", title.c_str()); fclose(f); }

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

    Serial.printf("[Web] 已保存 App: %s → \"%s\" (%u bytes)\n", name.c_str(), title.c_str(), code.length());
    g_appsDirty = true; // 通知主线程增量注册新 App(否则要重启才出现在应用列表)
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
    g_appsDirty = true; // 通知主线程注销已删 App
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
    // (原 killMonitorTask 已移除:Lua 停止改由 starboard_lua 的 LINE hook 接管,
    //  中键长按>1s / 无操作超时统一处理,无需独立监控任务)
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

// 主线程查询:Web IDE 是否有 App 增删(save/delete 置位)。返回 true 后应调 syncLuaApps,
// 再 clearAppsDirty。供 appWebIDE 主循环用——避免重启即可在应用列表看到新建/删除的 App。
bool appsDirty() { return g_appsDirty; }
void clearAppsDirty() { g_appsDirty = false; }

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
    luaSetCurrentApp(appName.c_str()); // 供 data.save/load 按 App 隔离

    char path[128];
    snprintf(path, sizeof(path), "/littlefs/apps/%s/main.lua", appName.c_str());

    lua_State *L = openLua();
    if (L)
    {
        Serial.printf("[Web] 运行 App: %s\n", appName.c_str());
        luaSysBeginRun(true); // Web IDE 在线运行:豁免超时深睡(连电脑保持唤醒);中键长按>1s 仍可停 Lua
        lua_execute(L, path);
        luaSysEndRun();
        closeLua(L);
    }

    isRunning = false;
    return true;
}