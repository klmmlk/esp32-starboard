# 正式板 SmartConfig / WiFi 连接失败排查

> 硬件：正式板（ESP32-S3 R8 + 8MB Octal PSRAM + MXIC DIO flash），固件分支 `fix/formal-board-stability`。
> 记录"设置 → 重新配网"连不上 WiFi 的排查过程、已排除的伪问题、已应用的修复、以及仍未定位的根因。**后人排查请先读「已排除的伪问题」一节，避免重复踩坑。**

---

## 1. 现象

在正式板上点「设置 → 重新配网」，串口日志（节选）：

```
I (7031) wifi:mode : sta (44:1b:f6:82:9b:60)
I (7035) HAL: WiFi 驱动已初始化
I (7037) HAL: 已配网,直连 SSID=正在获取IP地址
I (7069) wifi:new:<1,1>, old:<1,0>, ...
I (7071) wifi:state: init -> auth (0xb0)
I (8076) wifi:state: auth -> init (0x200)
I (8086) HAL: WiFi 断开, reason=2
I (8086) HAL: WiFi RSSI=0db
I (8087) HAL: WiFi 断开,退避 500ms 后重连(第 1 次)...
I (9830) wifi:new:<11,2>, old:<1,0>, ...      ← 信道从 1 跳到 11
I (9832) wifi:state: init -> auth (0xb0)
```

三个核心症状：

1. **没进 SmartConfig**：全程没有任何 `SC_EVENT` 日志，`已配网,直连` 说明 STA_START 走了"直连旧凭据"分支。
2. **连不上**：`reason=2`（AUTH_EXPIRE），`RSSI=0`（收不到 AP 的 beacon/auth 响应），auth→init 反复循环。
3. **跳信道**：信道 1 连不上后跳到信道 11 继续失败。

对照实验：**官方 SmartConfig 示例（`D:\test_official`）烧到同一块正式板、连同一个 AP，能成功连上并稳定停在信道 1**；手机在同一位置连该 AP RSSI ≈ -32dB，信号很好。

---

## 2. 已排除的伪问题（⚠️ 先读，别重踩）

排查中走过的弯路，记录在此防止重复：

| 误判 | 实际情况 | 证据 |
|------|----------|------|
| SSID `正在获取IP地址` 是 NVS 损坏 / 状态字符串误写 | **是真实 AP 名**，路由器 SSID 本身就叫这个 | 用户确认；全项目源码 grep 不到该字符串，但它由 `esp_wifi_get_config()` 从 NVS 正常读出 |
| 串口日志进设置后就消失，疑似 bug | **设计行为**。Arduino `Serial.println` 在 USB 未连接（电池运行）时会阻塞，必须用 `ESP_LOG*` | [starboard_display.cpp:8](../components/starboard_display/starboard_display.cpp#L8)、[starboard_app.cpp:15](../components/starboard_app/starboard_app.cpp#L15) |
| 进设置/重新配网触发 task watchdog | **字库死循环**：`u8g2_font_get_glyph_data` 查中文（wqy16）在 flash 20MHz DIO 下读不稳，死循环饿死 IDLE 喂狗。**已改 flash 40MHz 解决** | sdkconfig `CONFIG_ESPTOOLPY_FLASHFREQ_40M=y` |
| `esp_wifi_restore()` 是连不上的根因 | 去掉后能从 auth→init 进到 auth→assoc→init（进了一步），但 assoc 后仍 reason=4 失败。restore 不是根因，只是影响进度 | 旧日志 |
| `smartconfig busy` (12294) | SC 被启动两次（STA_START 自动分支 + 显式 start）。已用 `allowAutoSmartconfig` 标志抑制 | [hal.cpp](../components/starboard_hal/hal.cpp) |

---

## 3. 问题分层

这个失败其实是**两层独立问题**叠在一起，必须分开看：

### 层 A：重新配网没进 SmartConfig（流程 bug，已修复）

`wifiReprov()` 的设计是：清空 NVS → 冷重启 WiFi → STA_START 时检测到 NVS 空 → 自动走 SmartConfig 分支。见 STA_START handler 的判定逻辑：

```cpp
if (strlen((const char *)cfg.sta.ssid) > 0)   // 已配网 → 直连
else if (allowAutoSmartconfig)                  // 未配网 → SmartConfig
```

问题：`esp_wifi_restore()` / `esp_wifi_set_config(empty)` 在某些 driver 状态下会**静默失败**（代码原本没检查返回值），NVS 里的旧 SSID 没被清掉 → STA_START 误判"已配网"→ 直连旧凭据，**根本不进 SmartConfig**。

### 层 B：连不上 AP（`reason=2`，根因**仍未定位**）

无论直连旧凭据还是 SmartConfig 拿到新凭据，连接都在 auth 阶段失败（`reason=2` AUTH_EXPIRE，RSSI=0）。而官方示例同板同 AP 能连。**这才是真正的未解之谜，详见第 5 节。**

---

## 4. 已应用的修复

### 4.1 `reprovInProgress` 标志（修层 A）

在 [hal.cpp](../components/starboard_hal/hal.cpp) 加了 `reprovInProgress` 标志：`wifiReprov()` 置位后，下一次 STA_START **强制走 SmartConfig 分支，完全绕开对 NVS 是否清空的依赖**——不管 restore 成没成功都进配网。同时在 `wifiReprov()` 里打印 `set_config` / `restore` 的返回值和清空后的 NVS SSID，便于排查 restore 到底有没有成功。

修复后，重新配网的日志里**必须**出现这一行才算进了配网：

```
I (xxxx) HAL: STA_START(重新配网模式):强制 SmartConfig,忽略 NVS SSID='...'
```

⚠️ 此修复只解决层 A（让重新配网进 SmartConfig）。层 B 的 `reason=2` 见 4.4，已修复。

### 4.2 字库 watchdog（flash 20→40MHz）

`sdkconfig`：`CONFIG_ESPTOOLPY_FLASHFREQ_40M=y`。解决中文标题查字库死循环。

### 4.3 Serial → ESP_LOG

HAL 层日志全改 `ESP_LOGI/W`，保证电池运行时串口可见。

---

### 4.4 reason=2 根因修复：降 TX 功率 + 对齐官方 SC handler（**根因已定位并修复**）

`reason=2` AUTH_EXPIRE 的根因是 **TX 功率满功率（20dBm）时 auth 帧 TX 失真，AP 解析不了、回不了 auth 响应**。叠加启动日志里的 PHY 校准失败（`phy_init: failed to load RF calibration data (0x1102)`），校准失准时满功率失真更严重。修复（见 [hal.cpp](../components/starboard_hal/hal.cpp)）：

| 改动 | 作用 |
|------|------|
| `esp_wifi_set_max_tx_power(40)`（10dBm，`esp_wifi_start()` 后调用） | **核心修复**：避开 20dBm 失真区。参数单位 0.25dBm，40=10dBm |
| SC handler 对齐官方 `smartconfig_main.c:85-87`：`disconnect → set_config → connect` | sniffer 收完凭据后 STA 仍挂 sniffer 状态，不 disconnect 直接 connect 会 auth 失败。原代码注释误写"和官方一致没 disconnect"，实则官方有 |
| SC handler 设 `cfg.sta.bssid_set`（用 SmartConfig 拿到的 bssid） | 直连该 AP，避免 driver scan RSSI=0 找不到而跳信道 |
| `reason=2` 立即重连（最多 6 次），其他 reason 保持线性退避 | 对齐官方"STA_DISCONNECTED 无条件立即 `esp_wifi_connect()`"，磨过偶发 auth 超时 |

**验证**（降 TX 功率后直连，auth 一次成功）：

```
I (7040) HAL: WiFi TX 功率=40 (x0.25dBm=10dBm)
I (7057) wifi:state: init -> auth (0xb0)
I (7067) wifi:state: auth -> assoc (0x0)        ← 之前就在这里 reason=2 失败
I (7077) wifi:state: assoc -> run (0x10)
I (7100) wifi:connected ... channel 1, rssi: -49
```

> 为什么官方示例同板 20dBm 能连、产品代码 20dBm 不行：疑似产品代码每次启动 PHY 校准数据加载失败（0x1102）、回退运行时 full calibration，校准质量不如官方示例的干净 NVS，满功率输出失真。降功率是稳妥 workaround；根治可试 `idf.py erase_flash` 重生成 PHY 校准数据。

---

## 5. 根因小结

| 层 | 问题 | 修复 |
|----|------|------|
| A | 重新配网没进 SmartConfig（误走直连旧凭据分支，因 `esp_wifi_restore()` 静默失败） | `reprovInProgress` 标志强制 SmartConfig（4.1） |
| B | `reason=2` 连不上 AP | **降 TX 功率到 10dBm**（4.4 核心）+ 对齐官方 SC handler（disconnect/bssid）+ reason=2 立即重连 |

### 仍可优化（非阻塞）

- 启动日志的 `phy_init: failed to load RF calibration data (0x1102)`：每次 PHY 校准加载失败回退 full calibration，是满功率失真的帮凶。`idf.py erase_flash` 重烧可让校准数据从干净状态重新生成，届时或可把 TX 功率调回更高再验证。
- 10dBm 对近距离 AP（实测 -49dB）充足；若部署到 AP 较远场景，可试 12~15dBm（`esp_wifi_set_max_tx_power` 参数 = dBm×4）。
- 排查中曾怀疑过的 sdkconfig 差异 / PSRAM 总线竞争 / WiFi core 亲和性 / coex / PMF 等，已被"SmartConfig 阶段 RX 正常"排除——射频 RX 没问题，故障只在 TX 满功率失真。

---

## 6. 关键代码位置

| 位置 | 作用 |
|------|------|
| [hal.cpp — `wifiEventHandler`](../components/starboard_hal/hal.cpp) | WIFI/SC/IP 事件处理；STA_START 直连/SmartConfig 分支判定；`reprovInProgress` 强制配网 |
| [hal.cpp — `wifiEnsureInit`](../components/starboard_hal/hal.cpp) | NVS/netif/event loop/wifi_init/start，幂等初始化 |
| [hal.cpp — `wifiReprov`](../components/starboard_hal/hal.cpp) | 重新配网入口：清旧配置 → 冷重启 → 等 SmartConfig 结果 |
| [hal.cpp — `SC_EVENT_GOT_SSID_PSWD`](../components/starboard_hal/hal.cpp) | 收到手机发的 SSID/密码 → set_config + connect（对齐官方示例） |
| [appSettings.cpp — `wifiReprov()` UI](../main/apps/appSettings.cpp) | 设置里"重新配网"菜单项，确认后调 `hal.wifiReprov(180)` |
| [sdkconfig](../sdkconfig) | flash 40M / Octal PSRAM 80M / WiFi core0 / WiFi IRAM 等 |

---

## 7. 参考

- [ESP-IDF Wi-Fi Driver（reason code 官方含义）](https://docs.espressif.com/projects/esp-idf/en/v5.0.5/esp32s3/api-guides/wifi.html)
- [ESP-FAQ — Wi-Fi](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/wifi.html)
- [ESP32 reason 2 auth_expired 讨论（Arduino Forum）](https://forum.arduino.cc/t/esp32-c3-fails-to-connect-to-wifi-reason-2-auth-expired/1264358) — 网上对 reason=2 的"功率"解释其实**方向是对的**，只是要**降**功率（满功率 TX 失真）而非升功率（信号弱）。本例 AP 信号 -32dB，根因是 20dBm TX 失真（见 4.4）。
- [Espressif TX Power 配置指南](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/performance/modify-tx-power.html) — `esp_wifi_set_max_tx_power` 单位 0.25dBm、范围 [8,84]、须在 start 后调用。
- 官方对照示例：`D:\test_official`（`main/smartconfig_main.c`）
