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
| 左键 BUTTONL | 35 | ____ | ⚠️ S3 不可用 35，须换 RTC GPIO |
| 中键 BUTTONC | 34 | ____ | ⚠️ S3 不可用 34，须换 RTC GPIO |
| 右键 BUTTONR | 39 | ____ | ⚠️ S3 不可用 39 |
| 按键有效电平 | active-low（LiClock 自动检测） | ____ | 高/低电平触发 |
| 电池 ADC | PIN_ADC=33 | ____ | ⚠️ S3 不可用 33，换 ADC1 通道；分压比 |
| 充电状态 | PIN_CHARGING=26 | ____ | |
| SD 卡 CS/MOSI/MISO/SCLK | 14/12/13/15 | ____ | 或确认暂不用 SD |
| SD 卡检测 | 2 | ____ | 可选 |
| SD 电源控制 | 27 | ____ | 可选 |
| 蜂鸣器 | PIN_BUZZER=21 | ____ | LEDC 驱动 |
| I2C SDA/SCL | 23/22 | ____ | 传感器用（本工程暂无传感器） |
| Flash 总大小 | 8MB（LiClock factory 3MB + spiffs 1MB） | 8MB（默认/待确认） | 决定分区表大小；占位按 8MB |
| PSRAM | 已开 | 已开 ✓ Quad | 现有 sdkconfig 检测为 Quad；若 Octal 改 sdkconfig.defaults |

> 阶段 0 已在 `components/starboard_config/include/starboard_config.h` 填入**占位值**（屏幕引脚沿用已验证值，其余按 S3 安全引脚占位并标 ⚠️占位待改）。拿到你的实际接线后，**只改这一个头文件**即可，无需动其它代码。最关键待你确认的：**按键（左/中/右 GPIO）、电池 ADC、Flash 大小**。

**⚠️ 占位值的冲突提示**：`starboard_config.h` 里 I2C 占位用了 8/9、SD 占位用了 10/11，这些和屏幕引脚（DC=8/BUSY=9/CS=10/MOSI=11）冲突——仅占位让代码能编译，实际接线务必改成不冲突的引脚。

---

## 4. 分阶段计划

> 状态标记：⬜ 未开始 · 🔄 进行中 · ✅ 完成

### 阶段 0：工程基础与第三方库  🔄

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

### 阶段 1：HAL（按键 + 电源 + WiFi 配网 + NTP + 深睡）  ⬜

**目标**：硬件抽象层，参考 LiClock `src/hal.cpp`（806 行）+ `include/hal.h` 重写为 `starboard_hal`。

**参考**：`LiClock/src/hal.cpp`、`hal.h`、`battery.cpp`、`alarm.cpp`。

**Checklist**：
- [ ] 按键：OneButton 三键，保留 `hookButton`/`detachAllButtonEvents`/`task_hal_update` 轮询 tick
- [ ] 深睡/唤醒：移植 `goSleep`/`powerOff`/`set_sleep_set_gpio_interrupt`，适配 S3 RTC GPIO 唤醒约束，保留 `RTC_DATA_ATTR`
- [ ] **删除** LiClock 的 `refresh_partition_table()` + `test_littlefs_size()`（运行时魔改分区表，风险高），改用固定 partitions.csv
- [ ] WiFi 配网①：SmartConfig（`WiFi.beginSmartConfig`）
- [ ] WiFi 配网②：AP + DNS 劫持 + Web 配置页（`WiFiConfigManual` + `DNSServer`）
- [ ] WiFi 配网③：离线模式
- [ ] NTP：移植 `getTime` 时钟频率偏移补偿（`delta`/`every`/`lastsync`）+ sntp；无 DS3231，走纯 NTP 软件补偿
- [ ] 电压检测：`analogRead` 分压换算（S3 ADC1 引脚）
- [ ] 蜂鸣器 Buzzer：确认 arduino-esp32 3.x 的 LEDC API（`ledcAttachPin`/`ledcDetachPin`）
- [ ] Preferences 配置存取 + config.json (ArduinoJson)
- [ ] 验证：串口看按键事件；深睡按键唤醒恢复；AP 配网页可开

**风险**：S3 深睡唤醒引脚必须 RTC GPIO；arduino-esp32 3.x 的 LEDC/DNSServer/SmartConfig 逐项实测。

---

### 阶段 2：显示 + GUI（三色屏适配）  ⬜

**目标**：三色屏显示层与 GUI 工具，参考 LiClock `src/GUI.cpp` + `graph.cpp` + `include/GUI.h`。

**参考**：`LiClock/src/GUI.cpp`、`graph.cpp`、`include/GUI.h`。

**Checklist**：
- [ ] `starboard_display`：display 全局实例（`GxEPD2_3C<GxEPD2_420c_GDEY042Z98,...>`，引脚沿用 CS=10/DC=8/RST=7/BUSY=9）
- [ ] 三色屏策略：删 LiClock 局部刷新 `display.display(false)`，统一全屏 `display.display()`
- [ ] 语义颜色定义：`COL_NORMAL=GxEPD_BLACK`、`COL_ALERT=GxEPD_RED`、`COL_BG=GxEPD_WHITE`
- [ ] 红色约定：低电量/充电/闹钟/错误/天气预警→红，其余黑（GUI 层集中处理）
- [ ] 排版坐标从 296×128 → 400×300 重做
- [ ] 中文字体：`U8g2_for_Adafruit_GFX` + `u8g2_font_wqy12_t_gb2312`
- [ ] `starboard_gui`：移植 `msgbox`/`msgbox_yn`/`msgbox_number`/`msgbox_time`/`menu`/`drawWindowsWithTitle`/`drawLBM`/`autoIndentDraw`
- [ ] 验证：中文 msgbox + 红色渲染 + 菜单可切，全屏刷新无残影

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
| 3 | 三色屏只能全刷 | 删 partial；语义颜色；红色约定；坐标 400×300 重排 |
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

---

## 附：参考资源

- LiClock 源码：`LiClock/`（本仓库内，GPL-3.0）
- LiClock Wiki：https://github.com/diylxy/LiClock/wiki
- 现有 IDF 移植说明：`README_IDF.md`
- 已批准的原始计划：`~/.claude/plans/ticklish-snuggling-curry.md`
