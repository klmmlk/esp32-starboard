# esp32-starboard

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.x-blue)
![MCU](https://img.shields.io/badge/MCU-ESP32--S3-red)
![Display](https://img.shields.io/badge/Display-4.2%22%20Tri--color%20E--ink-black)
![Lua](https://img.shields.io/badge/scripting-Lua%205.4-purple)
![License](https://img.shields.io/badge/license-GPL--3.0-green)

> 基于 ESP32-S3 + 4.2 寸三色电子墨水屏的低功耗「墨水屏小电脑」,内置应用 + Lua 脚本 + Blockly 可视化编程,一次按键唤醒一次、平时微安级深睡待机。

**esp32-starboard**(CMake 工程名 `ink_test_idf`)是一块带拨轮三键、可充电、超低功耗待机的桌面小屏终端。架构参考并重写自开源项目 [**LiClock**](https://github.com/diylxy/LiClock)(@diylxy,ESP32-Solo-1 + 2.9" 黑白屏天气时钟,GPL-3.0),移植到 ESP-IDF(CMake)+ Arduino-as-component 栈,并升级为三色屏。

它既有一组内置应用(时钟 / 设置 / 首次引导 / OTA / Web 编程),也是一个**可编程平台**:用户能在同局域网的浏览器里用 Blockly 积木拖拽、或手写 Lua 脚本,开发自己的小应用并烧录进设备——无需安装任何工具链。

---

## ✨ 特性

- 🖥️ **4.2 寸三色墨水屏**(红/黑/白,400×300,GDEY042Z98),语义化配色(告警/低电/错误 = 红),全屏刷新避免串色残影。
- 🔋 **回合制 AppManager**:纯事件驱动 + 深睡,**一次按键 = 一次唤醒 = 一回合**,待机电流微安级。
- 🧩 **Lua 5.4 脚本运行时**:8 个内置模块覆盖 显示 / GUI / HTTP / 硬件 / 存储 / 音频 / 应用管理。
- 🧱 **Blockly 可视化 Web IDE**:浏览器打开设备 IP,积木编程 + 一键烧录/运行/删除 Lua App。
- 📶 **WiFi + SmartConfig(AirKiss/ESPTouch)一键配网**,微信小程序即可完成。
- ⬆️ **OTA 空中升级**(双 OTA 分区切换,ESP-IDF 原生 `esp_ota_*`)。
- ⏰ NTP 对时 + 冷启动从 NVS 兜底恢复时间 + 后台异步校时。
- 🔊 MAX98357A I2S 功放 + MP3 播放(libhelix 解码)。
- 💾 LittleFS 持久化(Lua 脚本 / App 数据 / MP3 资源)。
- 🀄 中文字体支持(wqy16 GB2312 全字库)。

---

## 🖥️ 硬件

### 规格

| 项目 | 规格 |
|------|------|
| 芯片 | **ESP32-S3**(Xtensa LX7 双核,160 MHz) |
| Flash | **16 MB**,DIO 模式,**40 MHz** |
| PSRAM | 正式板 **8 MB Octal** @ 80 MHz(可切 Quad / 关闭,见[板型变体](#-板型变体)) |
| 屏幕 | **4.2 寸三色墨水屏 GDEY042Z98**,400×300,红/黑/白,全刷约 5.4 s |
| 按键 | 拨轮三键(左/中/右),均为 RTC GPIO,可深睡唤醒 |
| 音频 | MAX98357A I2S 功放(单声道,免 MCLK) |
| 电源 | 深睡(`esp_deep_sleep_start`)+ EXT1 按键唤醒 + RTC TIMER 定时唤醒;屏幕 `hibernate()` 关驱动 IC 省电(双稳态内容保留) |

### 引脚定义

源:[`components/starboard_config/include/starboard_config.h`](components/starboard_config/include/starboard_config.h)。

| 功能 | GPIO | 说明 |
|------|------|------|
| 屏幕 MOSI | 11 | 已验证组,S3 默认 SPI |
| 屏幕 SCK | 12 | |
| 屏幕 CS | 10 | |
| 屏幕 DC | 8 | |
| 屏幕 RST | 7 | |
| 屏幕 BUSY | 9 | |
| 左键(数字-/返回) | 4 | RTC GPIO |
| 中键(确认) | 6 | RTC GPIO |
| 右键(数字+) | 5 | RTC GPIO |
| I2S BCLK | 13 | MAX98357A |
| I2S LRCLK | 14 | |
| I2S DIN | 17 | |

> ⚠️ `PIN_ADC=13`、`PIN_CHARGING=20`(电池/充电)、SD 卡、I2C 在配置头里均为**占位待改**,与上述已用引脚存在冲突,当前未启用;请按你的实际接线修改。选引脚时避开 S3 禁忌:GPIO26~32(Octal PSRAM)、33~37(SPI Flash)、0/3/45/46(strap)、19/20(USB)。

### 板型变体

代码**不做运行时自动检测**,靠编译前改配置切换。当前默认目标为**正式板**。

| 项 | 正式板(默认) | 自焊板(早期开发板) |
|----|--------------|---------------------|
| PSRAM | 8 MB Octal | 无 / Quad |
| Flash | 16 MB | 16 MB Quad |
| 按键电平 | active-high(`BUTTON_ACTIVE_LOW=false`) | active-low(`BUTTON_ACTIVE_LOW=true`) |
| 深睡唤醒沿 | EXT1 `ANY_HIGH` + 禁 RTC 内部上拉 | EXT1 `ANY_LOW` + 开 RTC 上拉 |

切换板型需改 4 处后重新编译:[`sdkconfig`](sdkconfig)(PSRAM Quad/Octal)、[`starboard_config.h`](components/starboard_config/include/starboard_config.h)(按键电平)、`hal.cpp`(深睡唤醒沿)、[`partitions.csv`](partitions.csv)(若 Flash 大小不同)。

---

## 📁 目录结构

```
.
├── main/
│   ├── main.cpp                 # app_main 入口
│   ├── apps.cpp / apps.h        # 内置 App 注册聚合
│   └── apps/                    # 每个 .cpp 一个内置 App
│       ├── appClock.cpp         # 时钟(默认 home)
│       ├── appSettings.cpp      # 设置
│       ├── appOOBE.cpp          # 首次开机引导
│       ├── appOTA.cpp           # OTA 升级
│       └── appWebIDE.cpp        # Web 编程(Blockly 服务端)
├── components/
│   ├── starboard_config/        # 引脚 / 尺寸 / 常量(无依赖,打破循环依赖)
│   ├── starboard_hal/           # 硬件抽象:串口/时区/NVS/按键/电压/充电/WiFi/NTP/深睡/LittleFS
│   ├── starboard_display/       # 三色屏封装 + 语义颜色 + 中文字体
│   ├── starboard_gui/          # msgbox / menu / waitKey 等控件
│   ├── starboard_app/          # AppBase + AppManager(回合制应用框架)
│   ├── starboard_audio/        # MAX98357A I2S,tone() / playMp3()
│   ├── lua/                     # Lua 5.4 运行时 + 模块绑定 + Web IDE 服务端
│   ├── GxEPD2/                  # ┐
│   ├── Adafruit_GFX/            # │ 需手动放入(见快速开始)
│   ├── Adafruit_BusIO/          # │
│   ├── U8g2_for_Adafruit_GFX/   # │
│   ├── OneButton/               # │
│   └── QRCode/                  # ┘
├── docs/                        # 开发与排障文档
├── partitions.csv               # 自定义分区表
├── sdkconfig                    # ESP-IDF 配置
└── CMakeLists.txt               # 工程根(project: ink_test_idf)
```

`managed_components/`(arduino-esp32、ArduinoJson、LittleFS、libhelix-mp3 等)由 ESP-IDF Component Registry 按 `idf_component.yml` 自动拉取,无需手动管理。

---

## 📱 内置应用

| 应用 | name | 功能 |
|------|------|------|
| 时钟 | `clock` | 默认 home。大号时间 + 中文日期星期 + 红色强调行;5 分钟定时唤醒兜底刷新。 |
| 设置 | `settings` | 屏幕方向(正常/反转 180°)、重新配网、无操作超时、默认应用、OTA 升级、关于。全部 NVS 持久化。 |
| 引导 | `oobe` | 首次开机:欢迎 → SmartConfig 配网 → NTP → 进主时钟;未完成则每次开机自动进引导。 |
| OTA 升级 | `ota` | 从配置 URL 下载固件,写 `ota_1` 分区后重启,带进度刷新。 |
| Web 编程 | `webide` | 连 WiFi 后启动 Blockly Web 服务器(80 端口),浏览器编程/保存/运行/删除 Lua App;10 分钟空闲超时。 |

> 「每日一字」这类是用户用 Lua/Web IDE 自建的脚本应用,不在 C++ 内置 App 中。

---

## 🛠️ 技术栈

| 类别 | 选型 |
|------|------|
| 构建 | ESP-IDF CMake(推荐 **v5.5.x**,兼容 `>=5.3, <6.1`) |
| Arduino | **arduino-esp32 3.3.10** 作为 IDF 组件(非 PlatformIO) |
| 脚本 | **Lua 5.4**(`LUA_32BITS`,32 位 float+int 省内存) |
| 显示 | GxEPD2 + Adafruit_GFX + U8g2_for_Adafruit_GFX(中文字体) |
| JSON | ArduinoJson 6.21.x |
| HTTP | ESP-IDF 原生 `esp_http_client` + `esp_crt_bundle`(HTTPS 内置证书包) |
| 文件系统 | LittleFS,分区 label `spiffs`,挂载点 `/littlefs` |
| 音频 | libhelix-mp3 + IDF `driver/i2s_std` |
| 按键 | OneButton |

---

## 🚀 快速开始

### 1. 环境准备

安装 **ESP-IDF**(推荐 v5.5.x),确认 `IDF_PATH` 已设置。`main/idf_component.yml` 已声明 `arduino-esp32@3.3.10` 与 `ArduinoJson`,会由 Component Registry 自动拉取。

### 2. 手动放置第三方库

将以下库放入 `components/`(它们不在 Registry,需手动管理):

- `Adafruit_BusIO`
- `Adafruit_GFX` —— **不要**拷 `fontconvert/` 和 `examples/`
- `GxEPD2`
- `U8g2_for_Adafruit_GFX`、`OneButton`、`QRCode`

### 3. 选择板型

按 [板型变体](#-板型变体) 调整 `sdkconfig`(PSRAM 模式)、`starboard_config.h`(按键电平)、`hal.cpp`(深睡唤醒沿)。

### 4. 构建与烧录

```bash
idf.py set-target esp32s3
idf.py menuconfig      # 确认 PSRAM / Flash size / 分区表
idf.py build
idf.py -p COMx flash monitor     # Ctrl+] 退出 monitor
```

> 改过分区表后,必须先整片擦除再烧录:`idf.py -p COMx erase-flash && idf.py flash`

<details>
<summary>用 Claude Code 辅助(可选)</summary>

仓库内 `.claude/skills/` 提供了封装好的辅助脚本:`build-idf`、`flash-idf`、`serial-monitor`、`memory-analysis`、`workflow`(串联编译+烧录+监控)。
</details>

---

## 🧩 Lua 应用开发

Lua App 存放在 LittleFS 的 `/littlefs/apps/<应用名>/` 目录:

| 文件 | 作用 |
|------|------|
| `conf.lua` | 元数据,设置全局变量 `title`(应用标题) |
| `main.lua` | 入口脚本,App 启动时执行 |
| `data.kv` | 运行期由 `data.save/load` 自动管理的键值持久化数据 |

### 内置模块

| 模块 | 主要 API |
|------|---------|
| `display` | `beginFrame/endFrame`、`clearScreen`、`setFont`、`drawPixel/Line/Rect/Circle/...`、`setCursor`、`setTextColor`、`u8g2Print`(中文)、`printWrapped` |
| `hal` | `VCC`、`isCharging`、`now`、`timeField`、`millis`、`reboot`、`wifiConnect`、`wakeupKey` |
| `gui` | `waitKey`、`tryGetKey`、`msgbox`、`msgbox_yn`、`msgbox_number`、`menu`、`drawLBM`、`drawBWBM`、`draw3ColorBM` |
| `appManager` | `gotoApp`、`goBack`、`setWakeupSec` |
| `http` | `get(url[,timeout])`、`jsonGet(body,key)`、`jsonArray(body,key)`(走 `esp_http_client` + 内置证书包,支持 HTTPS) |
| `data` | `save(key,value)`、`load(key)`(每个 App 独立 `data.kv`) |
| `sys` | `yield`(系统停止检查点,兼容旧脚本) |
| `audio` | `play(mp3路径)`、`beep(freq,ms)`、`volume(v)`、`stop`、`playing` |

### Web IDE(Blockly)

1. 打开内置 **Web 编程** App,设备连上 WiFi 后启动 HTTP 服务(80 端口)。
2. 同一局域网的浏览器访问 `http://<设备IP>/`,即 Blockly 积木编辑器(支持中文、含自定义 display/gui/image 积木,可生成 Lua)。
3. 工具栏:保存 / 打开 / 烧录(写 `main.lua`)/ 运行 / 删除;支持 MP3 上传。
4. 增删 App 后主线程增量同步,**无需重启**即可在应用列表看到。
5. **中键长按 >1 s** 可随时强停当前 Lua 脚本。

> Lua App 在独立的 FreeRTOS 任务(8192 栈)中运行;LINE hook 与各 yield 点接管「无操作超时 → 深睡」「中键长按 → 退出」。详见 [`docs/LUA_APP_SLEEP.md`](docs/LUA_APP_SLEEP.md)。

---

## ⚡ 架构要点:回合制 AppManager

与 LiClock 的常驻死循环不同,本项目采用**回合制**模型,把低功耗做到位:

- **一次唤醒 = `app_main` 重跑 = `appManager.run()` 一回合**,末尾 `goSleep()` 深睡,**不返回**。
- `run()`:靠 `RTC_DATA_ATTR lastAppName` 跨深睡恢复当前 App(深睡后栈丢失,只重建 home + current 两层)→ 处理系统手势(长按中键 → App 列表)→ 跑 `current->setup()` → **保持期**(默认 60 s 无操作超时;前 10 s 显示,>10 s 屏幕 `hibernate()`)→ `deepSleep()`。
- **App 的 `setup()` 返回 = 回合结束 = 进保持期 / 深睡**;定时唤醒(timer)跳过保持期直接深睡。

---

## 🔧 关键配置(sdkconfig)

| 配置 | 值 | 说明 |
|------|----|------|
| `CONFIG_IDF_TARGET` | `esp32s3` | 目标芯片 |
| Flash | `16MB` / `DIO` / `40MHz` | 曾因 20 MHz 查中文字库死循环触发 watchdog,提至 40 MHz |
| 分区表 | 自定义 [`partitions.csv`](partitions.csv) | nvs / otadata / factory / ota_0 / ota_1 / spiffs(LittleFS);当前按 8 MB 规划 |
| PSRAM | Octal 80 MHz,`USE_MALLOC`,`ALWAYSINTERNAL=16384` | >16 KB 分配走 PSRAM |
| 证书包 | `MBEDTLS_CERTIFICATE_BUNDLE=y`(DEFAULT_FULL,MAX 200) | Lua `http.get` 走 HTTPS 必需 |
| FreeRTOS HZ | 1000 | arduino-esp32 硬性要求 |

> WiFi:正式板满功率(20 dBm)TX 会失真导致断连(reason=2),代码内降到 10 dBm 修复。排障见 [`docs/SMARTCONFIG_WIFI_DEBUG.md`](docs/SMARTCONFIG_WIFI_DEBUG.md)。

---

## 📚 更多文档

- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) —— 分阶段开发计划与架构
- [`docs/LUA_APP_SLEEP.md`](docs/LUA_APP_SLEEP.md) —— Lua App 休眠 / 退出机制
- [`docs/SMARTCONFIG_WIFI_DEBUG.md`](docs/SMARTCONFIG_WIFI_DEBUG.md) —— 正式板 WiFi / SmartConfig 排障

---

## 📄 协议与致谢

本项目架构衍生自 [**LiClock**](https://github.com/diylxy/LiClock)(© @diylxy,**GPL-3.0**),按其协议开源:沿用 **GPL-3.0**,须保留原作者署名与协议声明,不可闭源商用。

> 许可证全文见 [`LICENSE`](LICENSE)。

第三方库各自遵循其原始协议(GxEPD2、Adafruit_GFX、U8g2、ArduinoJson、LittleFS、libhelix-mp3 等)。
