# esp32-starboard 开发文档

> 参考 [LiClock](https://github.com/diylxy/LiClock)（ESP32-Solo-1 + 2.9" 黑白墨水屏天气时钟）的架构，在 **ESP32-S3 + 4.2" 三色墨水屏** 的 ESP-IDF 工程上实现：分层 HAL + AppManager 应用框架 + Web 配网/OTA + 按键交互 + SD/LittleFS 文件系统。Lua 脚本运行时后置。

---

## 1. 项目背景与目标

### 现状

- 本工程 `esp32-starboard`：ESP-IDF（CMake）+ `arduino-esp32` 作为 IDF 组件引入；目标芯片 **ESP32-S3**；屏幕 **4.2" 三色墨水屏 GxEPD2_420c_GDEY042Z98（400×300，红/黑/白）**；当前 `main/main.cpp` 仅一个显示 "Welcome to" 的 hello-world。
- 参考工程 `LiClock/`：PlatformIO + Arduino；ESP32-Solo-1（单核 ESP32，非 S3）；2.9" **黑白**墨水屏（GxEPD2_290，296×128）；功能完备（HAL/AppManager/Lua/Web 配网/闹钟/传感器/电子书）。

### 目标

参考 LiClock 的架构，移植到本工程的栈上，按 ESP-IDF 惯例拆成多个 component 重写。

### 已确认的方向决策

| 决策项 | 选择 | 含义 |
|--------|------|------|
| 移植方式 | **参考重写** | 按 IDF 惯例拆 component，借鉴 LiClock 架构与大部分实现 |
| 三色屏红色 | **选择性用红色** | 重要信息/警告/强调用红，其余黑白 |
| Lua | **后置** | 先跑通 C++ App 框架，Lua 作为独立后续阶段 |

### ⚠️ 开源协议（务必遵守）

LiClock 采用 **GPL-3.0**，其 `README.md` 明确：「由此项目衍生出的代码也需要以 GPL-3.0 开源」。本项目参考并重写了 LiClock 的实现，属 GPL-3.0 衍生作品，**必须以 GPL-3.0 开源**，并在 LICENSE / README 中标明原作者（@diylxy）与工程链接。若需闭源商用，须改为「只学设计不抄码」的方式重新实现。

---

## 2. 目标工程结构

```
esp32-starboard/
├── CMakeLists.txt
├── partitions.csv                 # 阶段0：自定义分区表（含双 OTA 分区）
├── sdkconfig.defaults             # 阶段0：增补 Flash/PSRAM/分区表/LWIP
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml          # 阶段0：加 arduinojson / esp-async-webserver
│   └── main.cpp                   # 阶段3：app_main → hal.init() → 起 appManager 任务
├── components/
│   ├── Adafruit_BusIO/            # 已有
│   ├── Adafruit_GFX/              # 已有
│   ├── GxEPD2/                    # 已有（已含 gdey3c/ 三色 420c 驱动）
│   ├── starboard_config/          # 阶段0：引脚/常量/三色屏尺寸（替代 A_Config.h）
│   ├── starboard_hal/             # 阶段1：HAL
│   ├── starboard_display/         # 阶段2：display 全局实例 + u8g2 中文 + 三色屏包装
│   ├── starboard_gui/             # 阶段2：GUI namespace
│   ├── starboard_app/             # 阶段3：AppBase + AppManager + 内置 App
│   ├── OneButton/                 # 阶段0：库搬入
│   ├── U8g2_for_Adafruit_GFX/     # 阶段0：库搬入（中文字体）
│   ├── QRCode/                    # 阶段0：库搬入（配网页二维码）
│   ├── ESPAsyncWebServer/         # 阶段4（优先走 Component Registry）
│   └── lua/                       # 阶段5：Lua5.4 运行时（后置）
└── docs/
    └── DEVELOPMENT.md             # 本文档
```

每个新 component 自带 `CMakeLists.txt`，沿用 `components/GxEPD2/CMakeLists.txt`、`components/Adafruit_GFX/CMakeLists.txt` 的 `idf_component_register` + `REQUIRES` 写法。

---

## 3. ⚠️ 硬件参数待确认（启动阶段 0 的前置）

> LiClock 用 ESP32-Solo-1，引脚定义在 `LiClock/include/A_Config.h`。**S3 的 GPIO 分布与 ESP32 完全不同**，以下引脚必须重新确认。S3 关键约束：**GPIO26-32 连 Octal PSRAM、GPIO33-37 连 Quad SPI flash（不可用）**；深睡唤醒引脚必须是 RTC GPIO。

| 参数 | LiClock 值（仅供参考） | 本工程（待填） | 说明 |
|------|----------------------|---------------|------|
| 屏幕 CS | CONFIG_SPI_CS=5（当前 hello-world 用 10） | ____ | 当前已验证 CS=10/DC=8/RST=7/BUSY=9 可用 |
| 屏幕 DC | CONFIG_PIN_DC=5（当前用 8） | ____ | |
| 屏幕 RST | CONFIG_PIN_RST=18（当前用 7） | ____ | |
| 屏幕 BUSY | CONFIG_PIN_BUSY=19（当前用 9） | ____ | |
| 屏幕 SPI SCK/MOSI | 默认（当前 SCK=12/MOSI=11） | ____ | |
| 左键 BUTTONL | 4 | **4**（代码已定） | RTC GPIO |
| 中键 BUTTONC | 5 | **5**（代码已定） | RTC GPIO |
| 右键 BUTTONR | 6 | **6**（代码已定） | RTC GPIO |
| 按键有效电平 | active-low（LiClock 自动检测） | **active-low=true**（代码已定） | 高/低电平触发 |
| 电池 ADC | PIN_ADC=33 | ____ | ⚠️ S3 不可用 33，换 ADC1 通道；分压比 |
| 充电状态 | PIN_CHARGING=26 | ____ | |
| SD 卡 CS/MOSI/MISO/SCLK | 14/12/13/15 | ____ | 或确认暂不用 SD |
| SD 卡检测 | 2 | ____ | 可选 |
| SD 电源控制 | 27 | ____ | 可选 |
| 蜂鸣器 | PIN_BUZZER=21 | **40（有源，M6 暂缓）** | ⚠️ 有源 on/off，非 LiClock 无源 LEDC |
| I2C SDA/SCL | 23/22 | ____ | 传感器用（本工程暂无传感器） |
| Flash 总大小 | 8MB（LiClock factory 3MB + spiffs 1MB） | 8MB（默认/待确认） | 决定分区表大小；占位按 8MB |
| PSRAM | 已开 | 已开 ✓ Quad | 现有 sdkconfig 检测为 Quad；若 Octal 改 sdkconfig.defaults |

> 阶段 0 已在 `components/starboard_config/include/starboard_config.h` 填入**占位值**（屏幕引脚沿用已验证值，其余按 S3 安全引脚占位并标 ⚠️占位待改）。拿到你的实际接线后，**只改这一个头文件**即可，无需动其它代码。最关键待你确认的：**按键（左/中/右 GPIO）、电池 ADC、Flash 大小**。

**⚠️ 占位值的冲突提示**：`starboard_config.h` 里 I2C 占位用了 8/9、SD 占位用了 10/11，这些和屏幕引脚（DC=8/BUSY=9/CS=10/MOSI=11）冲突——仅占位让代码能编译，实际接线务必改成不冲突的引脚。

---

## 4. 分阶段计划

> 状态标记：⬜ 未开始 · 🔄 进行中 · ✅ 完成

### 阶段 0：工程基础与第三方库  ✅

**目标**：搭好分区表、配置、引脚定义、第三方库，让工程能在新结构下编译通过。

**参考**：`LiClock/include/A_Config.h`、`LiClock/mypartitions.csv`、现有 `components/GxEPD2/CMakeLists.txt`。

**Checklist**：
- [ ] 确认硬件参数（见第 3 节；占位值已填 `starboard_config.h`，待用户校准）
- [x] 新建 `partitions.csv`：nvs / otadata / phy_init / factory / **ota_0** / **ota_1** / spiffs(LittleFS)，按 8MB 规划
- [x] `sdkconfig.defaults` 增补：自定义分区表、Flash 8MB、`CONFIG_FREERTOS_HZ=1000`、PSRAM + malloc
- [x] 新建 `components/starboard_config/`：`include/starboard_config.h`（纯引脚/常量/`SCREEN_WIDTH=400/SCREEN_HEIGHT=300`，**不含** display/hal 的 include，打破循环依赖）+ CMakeLists
- [x] 搬入 `components/OneButton/`（LiClock/lib/OneButton/src 内容）+ CMakeLists
- [x] 搬入 `components/U8g2_for_Adafruit_GFX/`（src 内容）+ CMakeLists
- [x] 搬入 `components/QRCode/`（源码）+ CMakeLists
- [x] `main/idf_component.yml` 加 `bblanchon/ArduinoJson@^6`（与 LiClock 对齐，避免 v6/v7 API 差异）。ESPAsyncWebServer 等阶段4 再加，保持阶段0 最小
- [x] `main/CMakeLists.txt` + `main.cpp` 引用 `starboard_config`，验证组件可被 include
- [ ] **验证（需你在 ESP-IDF 命令行执行，本环境无 idf.py）**：
  - [ ] **⚠️ 先删旧的 `sdkconfig`**（现有 sdkconfig 是单 factory 分区表，优先级高于 defaults，不删则新分区表不生效）：在工程根删 `sdkconfig` 和 `sdkconfig.old`，或直接 `idf.py fullclean`
  - [ ] `idf.py set-target esp32s3`
  - [ ] `idf.py menuconfig` 抽查：Partition Table → Custom → 确认指向 `partitions.csv`；Flash size = 8MB；PSRAM 启用
  - [ ] `idf.py build` 通过
  - [ ] ⚠️ **首次烧录前必须先擦整片**（分区表变了）：`idf.py -p COMx erase-flash && idf.py -p COMx flash monitor`
  - [ ] 烧录后 hello-world（"Welcome to"）仍正常显示 → 阶段0 完成

**风险**：S3 引脚约束（见第 3 节）；旧 sdkconfig 覆盖新 defaults（验证步骤已处理）。

---

### 阶段 1：HAL（按键 + 电源 + WiFi 配网 + NTP + 深睡）  🔄(M1/M3/M4/M5 完成 · M2 跳过 · M6 暂缓)

**目标**：硬件抽象层，参考 LiClock `src/hal.cpp`（806 行）+ `include/hal.h` 重写为 `starboard_hal`。

**参考**：`LiClock/src/hal.cpp`、`hal.h`、`battery.cpp`、`alarm.cpp`。

**Checklist**：
- [x] 按键：OneButton 三键 + `task_hal_update` 轮询任务（20ms tick）。⚠️ `hookButton`/`detachAllButtonEvents` 属 App 层接口，留阶段3 AppManager 接管；当前 HAL 仅串口打印事件。
- [x] 深睡/唤醒：`goSleep()` = ext1 `ESP_EXT1_WAKEUP_ANY_LOW` + `rtc_gpio_pullup_en` + `RTC_PERIPH=ON` + `RTC_DATA_ATTR bootCount`。按 S3 官方睡眠文档重写（非照搬 LiClock 的 `ALL_LOW`）。
- [x] **删除** LiClock 的 `refresh_partition_table()` + `test_littlefs_size()`，改用固定 `partitions.csv`（工程从一开始就没移植这两个函数）。
- [x] WiFi 配网：**SmartConfig**（`SC_TYPE_ESPTOUCH_AIRKISS`，原生 `esp_smartconfig_*`，非 arduino `WiFi.beginSmartConfig`）。微信「乐鑫 AirKiss」小程序 / ESPTouch APP 推送。
- [ ] ⏭️ WiFi 配网② AP+DNS劫持 / ③ 离线模式：**弃用**（原计划①DPP 国产机扫不出、②AP 劫持需额外 APP，实测后均弃，最终选 SmartConfig；离线降级由 wifiInit 超时→用本地 RTC 时间显示覆盖）。
- [x] NTP：`esp_netif_sntp` 单源 `ntp.aliyun.com` + `sync_cb`。⚠️ 未移植 LiClock 时钟频率偏移软件补偿（`delta`/`every`/`lastsync`），纯 SNTP；长期漂移若明显再补。
- [ ] ⏭️ 电压检测：**跳过**（开发板暂无电池）。`VCC`/`USBPluggedIn`/`isCharging` 字段已留位，`update()` 待硬件就绪补采样。
- [ ] ⏭️ 蜂鸣器 Buzzer：**暂缓**（用户改接【有源】蜂鸣器 GPIO40，与 LiClock 无源 LEDC 频率驱动不同，播不了旋律；方案待定）。
- [x] Preferences：`pref.begin("starboard")` NVS 存取。⚠️ `config.json`(ArduinoJson) 暂无 App 消费，留阶段3。
- [x] 🔶 **新增（非 LiClock）**：WiFi 重连退避（线性 backoff，达 6 次停）+ `wifiInit(timeoutSec=8)` 阻塞等连接超时（防连不上旧 WiFi 挂死 app）。
- [x] 验证：✅ 串口看按键事件 / 深睡按键+定时双路唤醒恢复 / SmartConfig 配网→NTP 同步北京时间 / bootCount 跨深睡递增。

**状态**：M1/M3/M4/M5 已完成并烧录验证；M2（电池）/M6（蜂鸣器）因硬件待定暂缓跳过。HAL 主体完成，后续 App 消费时按需补 `config.json`、电压采样。

**风险**：S3 深睡唤醒引脚必须 RTC GPIO；arduino-esp32 3.x 的 LEDC/DNSServer/SmartConfig 逐项实测。

---

### 阶段 2：显示 + GUI（三色屏适配）  🔄(2a display + 2b GUI 移植完成 · 待烧录验证)

**目标**：三色屏显示层与 GUI 工具，参考 LiClock `src/GUI.cpp` + `graph.cpp` + `include/GUI.h`。

**参考**：`LiClock/src/GUI.cpp`、`graph.cpp`、`include/GUI.h`。

**Checklist**：
- [x] `starboard_display`：display 全局实例（`GxEPD2_3C<GxEPD2_420c_GDEY042Z98, HEIGHT>`）+ `display_init()`/`display_deinit()`，引脚 CS=10/DC=8/RST=7/BUSY=9。
- [x] 三色屏策略：统一全屏 `setFullWindow`/`firstPage`/`nextPage` 全刷，**不碰 partial**（`hasFastPartialUpdate=false`）。⚠️ 局刷路线经实测否决（见风险#3）：`nextPageBW`/局部刷新在 GDEY042Z98 上串色+残影+仍全屏闪，Waveshare 官方库亦无 partial API。
- [x] 语义颜色定义：`COL_NORMAL=GxEPD_BLACK`、`COL_ALERT=GxEPD_RED`、`COL_BG=GxEPD_WHITE`（`starboard_display.h`，本项目新增）。
- [x] 🔶 红色约定：GUI 层语义色 `COL_NORMAL`/`COL_ALERT`/`COL_BG` 已落地（starboard_gui 标题/告警可用 `COL_ALERT`）；「低电量/充电→红」等具体规则待 App 消费时按场景调用。
- [x] 🔶 排版坐标 296×128 → 400×300：主时钟骨架 + GUI（msgbox 280×190、menu 340×270、number/time 220×120）均已用 `SCREEN_WIDTH/HEIGHT` 居中重排。
- [x] 中文字体：`U8g2_for_Adafruit_GFX` + 默认主字体 `u8g2_font_wqy16_t_gb2312`（**16px**，库内唯一含 ASCII+全 GB2312 全字库）。
- [x] `starboard_gui`：移植 `msgbox`/`msgbox_yn`/`msgbox_number`/`msgbox_time`/`menu`/`drawWindowsWithTitle`/`autoIndentDraw`/`waitLongPress`。删 LiClock 的 `push_buffer`/`pop_buffer`（本项目 `GxEPD2_3C` **无** `swapBuffer`/`copyBuffer`/`current_buffer_idx`）→ 弹窗**不恢复背景**、返回后上层重画；每次画面变化全刷分页；颜色语义化；坐标 400×300；按键 `hal.btn*.isPressing()` 轮询 + `hal.pauseButtons` 暂停后台 tick 防 GUI 期间左键长按触发深睡。menu 刷新=**移动即全刷**（用户选定）。`drawLBM`/`fileDialog`（需 LittleFS）、`graph.cpp`（天气专用）后置。
- [ ] 🔶 验证：✅ 红色全刷渲染 + 中文显示 + 全刷无残影（main.cpp 主帧 demo 已验证）；中文 msgbox/msgbox_yn/menu 交互**待烧录**（main.cpp 中键唤醒进 `guiDemo`）。

**状态**：2a（display + 全刷策略 + 语义色 + 中文字体）完成（commit `1327c42`）；2b 事件驱动刷新 + `starboard_gui` 移植完成（**待编译+烧录验证**）。⏳ 待用户烧录报【全刷实测耗时】+ GUI 交互验证（msgbox/菜单全刷无残影）。

**风险**：三色屏不支持局部刷新（残影/串色）；坐标全重排。

---

### 阶段 3：AppManager 框架 + 内置 App  ⬜

**目标**：应用框架，参考 LiClock `include/AppManager.h` + `src/AppManager.cpp` + `src/apps/`。

**参考**：`LiClock/include/AppManager.h`、`src/AppManager.cpp`、`src/apps/`、`src/main.cpp`。

**Checklist**：
- [ ] `AppBase` 基类 + `AppManager` 栈式调度（近乎原样移植，纯 C++ 无芯片依赖）：`gotoApp`/`goBack`/`recover`/`appSelector`/`setTimer`，生命周期 setup/lightsleep/wakeup/exit/deepsleep
- [ ] `main.cpp`：`app_main` 起 `task_appManager` 跑 `appManager.update()`（替换 hello-world）
- [ ] OOBE 判定：`hal.pref.getInt("oobe",0)<=2` → 进引导
- [ ] 内置 App `appOOBE`（首次开机引导 + 配网）
- [ ] 内置 App `appSettings`（屏幕方向/NTP 间隔/夜间模式）
- [ ] 内置 App `appClock`（主时钟，先本地时钟，彩云天气 API 后置）
- [ ] 内置 App `appWebServer`（进 Web 配置/OTA，依赖阶段4）
- [ ] 验证：OOBE→配网→时钟→设置→Web 切换闭环；深睡唤醒恢复上次 App

**风险**：App 间依赖 display/hal/gui 的实例顺序，注意全局对象初始化。

---

### 阶段 4：Web 配网 + OTA  ⬜

**目标**：Web 配置页与空中升级。参考 LiClock `src/webserver/`，OTA 为新增。

**参考**：`LiClock/src/webserver/`（webserver.cpp + index.h/jss.h 等内嵌前端）。

**Checklist**：
- [ ] ESPAsyncWebServer：优先 Component Registry（`espressif/esp-async-webserver` + `async_tcp`）
- [ ] 路由：`/`（配置页）、`/setwifi`、`/settime`、`/upload`（上传到 LittleFS）、`/ota`（固件更新）
- [ ] 前端：沿用内嵌 `.h` 字符串常量法，按本项目设置项精简重写；Blockly 暂不搬
- [ ] OTA（新增）：`POST /ota` → Arduino `Update` 库写入 ota_0/ota_1 → `ESP.restart()`，配双 OTA 分区
- [ ] 配网二维码 QRCode 生成，坐标适配三色屏
- [ ] 验证：AP 模式手机连接，网页改 WiFi + 上传固件重启生效

**风险**：ESPAsyncWebServer 来源（Registry vs 源码）；OTA 分区切换。

---

### 阶段 5（后置）：Lua 脚本运行时  ⬜

**目标**：Lua 脚本能力。待阶段 0-4 跑通后单独立项细化。

**参考**：`LiClock/lib/lua`（Lua5.4 源码）、`src/lua/lua_trans.cpp`、`src/lua/modules/`、`src/luaAppWrapper.cpp`。

**Checklist**：
- [ ] `components/lua/`：Lua5.4 源码（保留 `luaconf.h` 的 `LUA_NUMBER=float`），GLOB_RECURSE 收录 .c
- [ ] **fopen 重定向（核心难点）**：`esp_vfs_register` 把 `/littlefs` 挂到 LittleFS，让标准 fopen 读 LittleFS；或改 `liolib.c`
- [ ] 移植 `lua_trans.cpp`（132 行胶水）+ `lua/modules/`（display/gui/hal/buzzer/appManager/http/weather 绑定）
- [ ] 移植 `LuaAppWrapper`：扫描 LittleFS `.app` 目录包成 App（conf.lua + main.lua + icon.lbm）
- [ ] Blockly（图形化生成 Lua）：最后做或不做
- [ ] 验证：放一个 `.app`，Lua 脚本能跑并调显示

**风险**：fopen 重定向是最大难点；Lua 堆用 PSRAM（依赖 `CONFIG_SPIRAM_USE_MALLOC`）。

---

## 5. 关键风险与决策清单

| # | 风险/点 | 处置 |
|---|--------|------|
| 1 | GPL-3.0 衍生约束 | 本项目须 GPL-3.0 开源；LICENSE/README 标明原作者与链接 |
| 2 | 引脚不兼容（S3 vs Solo-1） | 阶段0 确认按键/SD/ADC 引脚，集中 `starboard_config`；26-37 受限 |
| 3 | 三色屏只能全刷 | 删 partial；语义颜色；红色约定；坐标 400×300 重排。⚠️ 进一步(阶段2a 实测)：SSD1683 双显存(0x24黑白/0x26红白)的刷新波形**硬件层不可分离**——`refresh()`走全色全刷(~25s)，`refresh_bw()`只刷黑白但红色会变浅/黑且仍是整屏刷、与 `GxEPD2_3C` 分页架构冲突。**结论:黑/红刷新不分开,坚持"选择性用红"(红只用于低频切换的静态强调元素)**。 🔴**进一步实测(06-24)**:`nextPageBW`+`setPartialWindow` 黑白局刷在 GDEY042Z98 上【不可用】:① 即便设局部区域仍【整屏闪】(420c 驱动底层 full window refresh);② 局刷时屏上已有【红色被黑白波形洗淡/串色】;③ 局刷区【残影累积】。Waveshare 官方 `4in2b_V2` 库亦无 partial API(只有 Init/Clear/Display/Sleep 全刷)。**→ 局刷路线彻底否决,本项目锁定全彩全刷;刷新策略定为【纯事件驱动】(平时深睡静态保持,仅按键/天气更新/闹钟触发才全刷),放弃 LiClock 那套"每分钟局刷更新分钟数"。** |
| 3a | 中文字体只有 wqy 可靠(阶段2a 实测) | 库内**唯一**同时含 ASCII+全 GB2312 汉字的是 `u8g2_font_wqy*_t_gb2312` 系列(12-16px,~318KB)。`unifont_tf`/`crox*c_tf` 仅几KB=纯拉丁无汉字；`b10/12/16_t_japanese*`=日文汉字(非中文)；`unifont_t_chinese1/2/3`=只含汉字不含ASCII需分3段。默认用 `wqy16`。点阵细体,软件横向描边加粗(weight)实测**不如原样清晰**,故不加粗。 |
| 4 | 运行时魔改分区表 | 删 `refresh_partition_table`/`test_littlefs_size`，用固定 partitions.csv |
| 5 | arduino-esp32 3.x API | LEDC（新 API）/DNSServer/SmartConfig 在 S3 上逐项实测 |
| 6 | S3 深睡唤醒约束 | 唤醒按键必须 RTC GPIO；ext0/ext1 按 S3 调整 |
| 7 | Lua fopen（阶段5） | `esp_vfs_register` 挂 LittleFS 或改 liolib |
| 8 | ESPAsyncWebServer 来源 | 优先 Component Registry，否则搬源码到 components |
| 9 | OTA（LiClock 无） | 新做：Arduino Update 库 + 双 OTA 分区 |

---

## 6. 进度日志

> 每次推进时在此追加一条。格式：`YYYY-MM-DD —— 内容`

- 2026-06-23 —— 完成对 LiClock 源码的架构调研（AppManager/Lua/GUI/HAL/外设/Web），确认移植方向（参考重写 / 选择性用红 / Lua 后置），编写本开发文档。
- 2026-06-23 —— **阶段0 文件就绪**（待 build 验证）：新建 `partitions.csv`（8MB/双OTA）、更新 `sdkconfig.defaults`、新建 `starboard_config`（引脚占位）、搬入 OneButton/U8g2_for_Adafruit_GFX/QRCode 三库、main 引用 config、加 ArduinoJson@6。⚠️ 发现并记录"旧 sdkconfig 覆盖 defaults"坑，验证步骤要求先删 sdkconfig。🚧 卡点：本环境无 `idf.py`，build 验证需用户在 ESP-IDF 命令行执行。
- 2026-06-23 —— build 验证：IDF v5.5.4 + arduino-esp32 3.3.10 版本匹配 ✓，所有 component 正确识别 ✓。**首次 build 报错**：`partitions.csv` 里 `phy_init` 分区(0xf000)与 `otadata`(0xe000~0x10000)重叠。**已修复**：删除 phy_init 分区（arduino-esp32 默认把 PHY 数据编译进固件，不需单独分区）。6 分区严格连续填满 8MB。
- 2026-06-23 —— **二次 build 报错**：QRCode 库 `qrcode.c` 的 `#pragma mark`（Apple 专用 pragma）+ `type-limits` 被 IDF 默认 `-Werror=all` 升级为错误。
- 2026-06-23 —— **三次 build 仍报错**：先试 `target_compile_options(... -Wno-error)` 不生效——GCC 里**裸 `-Wno-error` 压不住 `-Werror=<具体类型>`**（如 `-Werror=unknown-pragmas`）。**正确修法**：用 `-Wno-unknown-pragmas -Wno-type-limits` 等具体的 `-Wno-<type>`，从根上消除该警告（不依赖 `-Werror` 压制）。QRCode 已改用此法。⚠️ 教训：以后第三方库报 `[-Werror=xxx]`，一律用 `-Wno-xxx` 对症，别用裸 `-Wno-error`。
- 2026-06-23 —— ✅ **阶段0 build 通过**（IDF v5.5.4 + arduino-esp32 3.3.10 + S3）。所有组件（starboard_config/OneButton/U8g2/QRCode/ArduinoJson/GxEPD2）编译成功。剩烧录验证：分区表已改，需 `erase-flash` 后重烧确认 hello-world 正常。
- 2026-06-23 —— ✅ **阶段0 烧录验证通过**，hello-world 正常显示。阶段0 完成，提交 commit `ae8a7d7`。
- 2026-06-23 —— **阶段1 启动（HAL）**。设计决策：`starboard_hal` **不依赖 display/gui**（保持底层纯粹），配网/关机等 UI 交互留给阶段3 App。分 6 个里程碑小步推进：M1 init+按键 / M2 配置+电压 / M3 WiFi / M4 NTP / M5 深睡 / M6 蜂鸣器。重要移植点：LiClock 的 Buzzer/hal 的 LEDC 调用是 arduino-esp32 **2.x API**（`ledcSetup`/`ledcAttachPin(pin,ch)`），3.x 已改（`ledcAttach(pin,freq)` / `ledcWriteTone(pin,...)`），蜂鸣器移植时必须适配。
- 2026-06-23 —— **阶段1-M1 完成**（待编译验证）：`starboard_hal` 组件骨架 + `init()`（串口/时区/Preferences/按键轮询任务）+ OneButton 三键事件串口打印。main.cpp 调 `hal.init()`。
- 2026-06-23 —— ✅ **阶段1-M1 验证通过**（编译 + 烧录）。修一处编译错：OneButton 库**无 `isPressed()`**,应为 `isPressing()`(实时 `digitalRead` 引脚电平)。`waitForAllReleased()` 用它判断「键是否还按着」一直等到全松开。⚠️ 教训:本项目 OneButton 版本的 API 是 `isPressing()`/`isLongPressed()`/`isIdle()`,移植 LiClock 时注意别照抄别处的方法名。
- 2026-06-23 —— ✅ **阶段1-M5 完成(深睡 + ext1 按键唤醒)**。按 ESP32-S3 官方睡眠文档重写(非照搬 LiClock):ext1 + `ESP_EXT1_WAKEUP_ANY_LOW`(S3 正解;LiClock 的 `ALL_LOW` 已 deprecated 且语义是「全部为低」)、`rtc_gpio_pullup_en` + `RTC_PERIPH=ON`(数字 GPIO 的内部上拉深睡时随数字域断电失效,必须另开 RTC IO 上拉,否则引脚浮空唤醒不可靠)、`RTC_DATA_ATTR bootCount` 验证 RTC 内存跨深睡保留。验证:长按左键进深睡、任一键唤醒(GPIO 号正确)、bootCount 递增。
- 2026-06-23 —— M5 踩的坑(均含教训):① **`uint32_t` 在 xtensa 是 `unsigned long`**,`printf` 用 `%u` 触发 `-Werror=format=` → 用 `(unsigned)` 转型。② **按键回调里直接调 `goSleep` → 内部 `waitForAllReleased` → `tickButtons` 重入 `OneButton::tick`,长按回调被递归重复触发,叠加 `esp_sleep_*` 栈消耗,4096 任务栈溢出 panic 重启**(现象像「自动唤醒」,实为栈溢出重启——输出里没有第二次 init 横幅可辨)。改法:回调只置 `wantSleep` 标志,任务循环在 `tick()` 返回后的干净栈上调 `goSleep`;任务栈 4096→8192。⚠️ 教训:OneButton 回调里别跑重操作,一律用标志位 deferred 到任务主循环。③ **硬件接线:屏幕 CS(10)/BUSY(9) 插反** → 屏不工作/`display.init()` 卡死(BUSY 是屏输出、CS 是输入,插反 SPI 时序全乱)。已修正。
- 2026-06-23 —— ✅ **阶段1-M3/M4 完成(WiFi 配网 + NTP 对时)**。配网选型:先做 **DPP**,实测国产 Android **扫不出配网**(系统相机把 `DPP:` URI 当文本,国产 ROM 无 Easy Connect 入口)→ 弃用;`wifi_prov_mgr` SoftAP 需手机装官方 APP(用户偏好不装)→ 弃;最终选 **SmartConfig(`SC_TYPE_ESPTOUCH_AIRKISS`)**:手机用**微信「AirKiss」小程序**(国产机人人有微信、无需装额外 APP)或 ESPTouch APP,UDP 广播把 SSID+密码发给设备,设备侦听接收。NTP 用官方 `esp_netif_sntp`(单源 `ntp.aliyun.com`,GOT_IP 后启动)。验证:微信小程序配网→设备收 SSID→连接→NTP 同步北京时间→重启自动直连。
- 2026-06-23 —— M3/M4 坑(均含教训):① **DPP 链接错**(`esp_supp_dpp_*` undefined):`CONFIG_ESP_WIFI_DPP_SUPPORT=y` 改了 sdkconfig.defaults **没 reconfigure**,已有 sdkconfig 优先级更高、wpa_supplicant 没编译 esp_dpp.c。⚠️ 印证阶段0 教训:改 sdkconfig.defaults 必须 fullclean/reconfigure。② **NTP 多源编译错**:`ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE` 传 2 服务器报「too many initializers」——`servers[]` 大小受 `CONFIG_LWIP_SNTP_MAX_SERVERS` 限制(默认 1),改单源 `ESP_NETIF_SNTP_DEFAULT_CONFIG`。③ **SmartConfig 用原生 IDF `esp_smartconfig_*`**(在 esp_wifi 组件),非 arduino WiFi 库封装(后者 3.x 已移除)——别被「3.x 移除 SmartConfig」误导。④ 凭据存 **esp_wifi 默认 NVS**(`esp_wifi_get_config` 判已配网),不用 pref。⚠️ 教训:配网方案先验证**目标手机生态**(国产机:微信小程序/不装 APP 优先),别只看协议先进性(DPP 官方推荐但国产机不支持)。
- 2026-06-24 —— **阶段1 收尾决策 + 阶段2 启动**。① 蜂鸣器(M6):用户改接 **有源蜂鸣器(GPIO40)**——与 LiClock 无源(LEDC 频率驱动)驱动方式完全不同(有源只能 on/off、播不了旋律),原计划方案推翻;经讨论用户决定**暂时跳过 M6**,留到硬件/方案定了再做。② 电池(M2 电压检测):**开发板暂无电池,跳过**;M2 的配置持久化部分无 App 消费也暂搁。③ 据此阶段1 暂停于"M1/M3/M4/M5 已完成",转入**阶段2(显示+GUI)**——它不依赖电池/蜂鸣器,屏幕引脚已验证,可独立推进。
- 2026-06-24 —— **阶段2a 完成**(`starboard_display` 组件,待编译验证)。把原散在 `main.cpp` 的 display 实例抽成独立 IDF 组件:`GxEPD2_3C<GxEPD2_420c_GDEY042Z98, HEIGHT>` 全局实例 + `U8G2_FOR_ADAFRUIT_GFX u8g2` + 统一全刷初始化 `display_init()`/`display_deinit()`。新增**语义颜色**(本项目新增,LiClock 黑白屏无红色):`COL_NORMAL=黑/COL_ALERT=红/COL_BG=白`。中文字体默认 `u8g2_font_wqy16_t_gb2312`(16px;订正:此前误写 15px,代码 `CN_FONT_MAIN` 实为 16px)。main.cpp 改为验证 demo:主时钟骨架(顶部黑大字时间 + 中部黑中文 + **红色**预警中文 + 底部状态栏)。main/CMakeLists REQUIRES 由 `GxEPD2` 换成 `starboard_display`(后者 PUBLIC 传递 GxEPD2/U8g2/Adafruit_GFX)。
- 2026-06-24 —— **阶段2 刷新策略定案(纯事件驱动)**。① 局刷实测否决(见风险#3)。② 派 agent 调研 LiClock 刷新机制:LiClock 是【深睡 + 定时/按键唤醒 + setup()画一帧】的**事件驱动架构**(`loop()` 自删,逻辑跑在 `task_appManager` 状态机),**非后台定时刷屏**——这层可照搬,三色屏全刷的慢可被深睡掩盖。但其【主时钟每分钟局刷更新分钟数】(`appClock.cpp:198` `display(true)`)与【GUI 菜单每按一次键局刷重画选中框】(`GUI.cpp:246` `displayWindow`)是局刷核心用途,三色屏**必须改造**:放弃分钟实时刷新、菜单改"浏览不刷/确认才刷"、`push_buffer/pop_buffer` 缓冲区栈因 GxEPD2_3C 不支持 `swapBuffer` 改为整屏重画。③ 经确认采用【**纯事件驱动**】刷新:平时深睡,屏幕静态保持(墨水屏断电保持特性),仅按键/天气更新/闹钟触发全刷一帧。④ main.cpp 改为纯事件驱动全刷验证 demo(`refreshMainFrame()` 全刷一帧 + millis 测耗时 → `goSleep(0)` 纯按键唤醒),用于实测全刷耗时(后续所有交互的时间基准)。待用户烧录报实测耗时。
- 2026-06-24 —— 阶段2a 调研要点(经源码 + context7 GxEPD2 官方文档双重核实):① **三色屏全刷模式**与官方 GxEPD2_GFX_Example 一致(`setFullWindow`→`firstPage`→`do{ 绘制 }while nextPage`),绘制须在循环体内**每页重画**。② **420c 三色屏底层全刷**:`GxEPD2_420c_GDEY042Z98.h` 注释明示"has partial window addressing, but uses full window refresh",`hasFastPartialUpdate=false`→ 本项目**不碰 partial**,无需额外禁用。③ **`display(false)` 是全屏不是局部**:LiClock 调的 `display.display(false)` 已是全屏(参数=是否局部),真正要避免的是 partial/带 true 的局部刷;文档原"删 display.display(false)"表述已更正。④ **u8g2 颜色独立**:与 `display.setTextColor` 互不相干,画中文前须各自 `u8g2.setForegroundColor`;`drawUTF8(x,y,str)` 显式带坐标、不依赖游标。⑤ 字体符号声明在 `U8g2_for_Adafruit_GFX.h`,cpp 无需额外 include `u8g2_fonts.h`。
- 2026-06-24 —— **阶段1 WiFi 健壮性补强**(已进 commit `1327c42`)。① 连接阻塞+超时:`wifiInit(timeoutSec=8)` 轮询 wifiState,连上/配网中即返回,超时设 `Failed` 并把退避计数顶到上限停重连(防"连不上旧 WiFi → 反复重连刷屏/耗电 → app 无限挂死")。② 断线线性退避:`WIFI_EVENT_STA_DISCONNECTED` 上按失败次数 0.5s/1s/1.5s… 递增 backoff,达 6 次停(密码错/找不到 AP 持续失败时让设备消停,等下次唤醒再试)。③ 连上(GOT_IP)清零失败计数。⚠️ 教训:墨水屏+深睡设备连不上网不能疯狂重试(刷屏+耗电+卡死 app),必须有【超时放弃+退避】兜底。
- 2026-06-24 —— **阶段2b 前置:main.cpp 演进为【事件 + 定时兜底】混合刷新 demo**。在原"纯事件驱动全刷"基础上:① 加定时兜底(`REFRESH_INTERVAL_SEC=15min`),`goSleep(sec)` 同时开 timer+按键双路唤醒(先到先触发),挂着不动时信息也不至于过时太久;② 配网模式(`wifiState==Provisioning`)**保持唤醒不睡**(否则用户来不及用微信推 WiFi),刷配网提示页后轮询最长 5 分钟等 `Connected`;③ 时间有效性改判 `tm_year>120`(深睡 RTC 维持走时,即便本次没连上 NTP 只要对过时就有准时间),不再只依赖 `timeSynced`。⏳ **待用户烧录报【全刷实测耗时】**(firstPage~nextPage 段,不含 WiFi),据此定交互响应节奏/兜底间隔。
- 2026-06-24 —— **阶段2b starboard_gui 移植完成**(待编译+烧录验证)。新建 `components/starboard_gui/`(`.h`/`.cpp`/CMakeLists),移植 LiClock `GUI.cpp` 的 msgbox/msgbox_yn/msgbox_number/msgbox_time/menu/drawWindowsWithTitle/autoIndentDraw/waitLongPress。三色屏改造:① 删 push_buffer/pop_buffer——本项目 `GxEPD2_3C` **全库无** `swapBuffer`/`copyBuffer`/`current_buffer_idx`(grep 确认:LiClock 那版 GxEPD2 有、本项目版没有),故弹窗【不恢复背景】、返回后上层重画;② 每次画面变化用 `setFullWindow`/`firstPage`/`nextPage` 全刷分页;③ 颜色硬编码 0/1→COL_NORMAL/ALERT/BG;④ 坐标 296×128→400×300 居中、窗口放大(msgbox 280×190/menu 340×270);⑤ 按键 `hal.btn*.isPressing()` 阻塞轮询。⚠️ **关键坑**:hal 的 `task_hal_update` 是**独立任务**,GUI 阻塞轮询期间后台仍 tick,用户在 GUI 里按左键(否/减)稍久会触发长按回调→`wantSleep`→深睡打断交互——给 HAL 加 `volatile bool pauseButtons` 字段,`task_hal_update` 门控(GUI 期间跳过 tick+深睡),starboard_gui 进出阻塞函数成对切换;`isPressing()` 不受影响(实时 digitalRead)。menu 刷新策略经确认=**移动即全刷**(实时可见,代价每次按键等一次全刷;用户接受,若实测太慢再回头改)。drawLBM/fileDialog(需 LittleFS)、graph.cpp(天气专用)本轮后置。main.cpp 加 `guiDemo`:中键唤醒→msgbox→msgbox_yn→menu→刷回主帧。⚠️ 待用户 `idf.py build` + 烧录验证(本环境无 idf.py);顺带报【全刷实测耗时】。若编译报 `Fonts/FreeSans9pt7b.h` 找不到,给 `starboard_gui/CMakeLists` 加 `REQUIRES Adafruit_GFX`(msgbox_number/time 数字字体经 Adafruit_GFX,正常经 starboard_display→GxEPD2→Adafruit_GFX 传递可见)。

---

## 附：参考资源

- LiClock 源码：`LiClock/`（本仓库内，GPL-3.0）
- LiClock Wiki：https://github.com/diylxy/LiClock/wiki
- 现有 IDF 移植说明：`README_IDF.md`
- 已批准的原始计划：`~/.claude/plans/ticklish-snuggling-curry.md`
