# Lua App 休眠根治方案

> 解决「Web IDE 生成的 Lua App 无法被系统休眠机制接管」的架构级问题。本文是设计与跟进文档，风格对标 [`DEVELOPMENT.md`](./DEVELOPMENT.md)，状态标记同：⬜ 未开始 · 🔄 进行中 · ✅ 完成。
>
> **一句话方案**：保留 Lua 专用任务（8192 栈规避爆栈），但把「无操作超时 → 深睡」和「中键长按 >1s → 退出」的判断直接做进 Lua 的 LINE hook 与各 yield 点，**让 Lua 任务自己停自己**，再通知 `setup()` 返回——控制流回归单线程，系统休眠机制重新接管。

---

## 1. 背景与问题

### 1.1 系统休眠模型：回合制深睡

本工程的休眠是**回合制**的（见 [`DEVELOPMENT.md`](./DEVELOPMENT.md) 阶段 3）。一次唤醒 = `app_main` 重跑 = `appManager.run()` 跑一回合：

```
恢复 current → 系统手势 → current->setup() → 消费 pending 切换 → 记 lastAppName
                         │
                         └─ setup() 返回后 ──► 保持期（无操作超时计时）─► deepSleep()
                                                                  （芯片深睡，RAM 全丢）
```

**铁律：`setup()` 必须返回，才会进入「保持期」，才会 `deepSleep()`。**

保持期逻辑在 [`starboard_app.cpp:236-297`](../components/starboard_app/starboard_app.cpp#L236)（保持 `sleep_to` 秒、前 10s 显示、10s 后 `display.hibernate()`、按键重画/列表、超时后深睡），它在 `current->setup()` 返回**之后**才执行。内置 App（Clock/Settings/OOBE）的 `setup()` 是「画一帧 + GUI 阻塞交互」后返回，所以休眠正常。

### 1.2 病根：Lua App 的 setup 不返回

Lua App 的 [`lua_app_wrapper.cpp:180`](../components/lua/lua_app_wrapper.cpp#L180) `LuaApp::setup()` 用「创建后台任务跑 Lua + 阻塞等通知」实现：

```cpp
xTaskCreate(luaAppTask, "LuaApp", 8192, ...);          // Lua 跑在独立后台任务
uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));  // setup 阻塞最多 30s
```

由此产生**三大病灶**：

| # | 病灶 | 表现 |
|---|------|------|
| ① | **控制流割裂（根因）** | Lua 是长驻交互式（while 循环等按键），但休眠计时器在主任务的保持期里；`setup()` 卡在 `ulTaskNotifyTake` 等后台任务，主任务挂起 → **保持期永不运行 → 无操作超时彻底失效 → 不能深睡**。`sys.yield()` 注释声称能推进计时器，实为误导（它在后台任务里 `delay(1)`，主任务仍挂起）。 |
| ② | **30s 硬超时 + 任务泄漏** | 超时后 `setup()` 强制返回进保持期，但**后台 Lua 任务还活着**，继续调 display/GUI，和保持期的 `display.hibernate()`/重画**抢屏幕**（竞态）；最后 `deepSleep()` 强杀任务，可能留半截屏操作。超时分支只设 `timedOut` 禁止 `goBack`，**并没有真正停掉 Lua 任务**。 |
| ③ | **停止机制没接系统侧** | 基础设施其实已齐（见下），但全仓唯一调用 `requestLuaStop()` 的是 [`lua_webserver.cpp:50`](../components/lua/lua_webserver.cpp#L50) 的 `killMonitorTask`（Web IDE 编程时中键长按 3s 强停），**与 App 运行时的休眠毫无关系**。没有任何机制在 Lua App 运行期检测「无操作超时」或「退出手势」。 |

### 1.3 已有基础设施（方案要复用，不重造）

[`starboard_lua.cpp`](../components/lua/starboard_lua.cpp) 已具备从 Lua 死循环干净跳出的能力：

- `requestLuaStop()` / `isLuaRunning()` / `luaStopRequested()` + `g_luaStopRequested` 标志（[L33-38](../components/lua/starboard_lua.cpp#L33)）。
- `luaStopHook`（[L40](../components/lua/starboard_lua.cpp#L40)）：LINE hook，每行 Lua 触发；检查停止标志 → `luaL_error` **长程跳转**（能从 Lua 死循环干净跳出）；每 2000 行 `vTaskDelay(1)` 喂狗。
- `lua_execute` / `lua_execute_string` 入口 `lua_sethook(L, luaStopHook, LUA_MASKLINE, 0)`。
- `common_delay`（[L71](../components/lua/starboard_lua.cpp#L71)）分段 delay，每 10ms 检查停止标志 + `GUI::pollInputs()`。
- [`lua_gui.cpp`](../components/lua/lua_gui.cpp) 的 `waitKey`/`waitButton`/`tryGetKey`（[L30-68](../components/lua/lua_gui.cpp#L30)）已轮询检查停止标志。

> **结论**：缺的不是「能不能停 Lua」，而是「谁来在 App 运行时按系统规则（超时/退出）触发停止」。这正是本方案要补的。

---

## 2. 关键约束（已确认，决定方案形态）

| 约束 | 值 | 影响 |
|------|----|------|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | **3584** 字节（[`sdkconfig:1540`](../sdkconfig#L1540)） | Lua 需 8192 栈 → **不能把 Lua 挪到 app_main 主任务**（复杂 Blockly 脚本 + display 全刷有爆栈风险）。故保留专用任务。 |
| Lua 后台任务栈 | `LUA_TASK_STACK = 8192` | 保留。 |
| Task WDT | `TIMEOUT_S=5`，只监控 IDLE 任务（无用户任务订阅） | hook 每 2000 行 `vTaskDelay(1)` 喂狗已足够；**不要**给 Lua 任务加 WDT 订阅（会 panic）。 |
| 两条 Lua 执行路径 | ① App 列表 → `LuaApp::setup()`（后台任务，病根）；② Web IDE `/api/run` → `pollRunRequest()`（已在主任务同步，[`lua_webserver.cpp:977`](../components/lua/lua_webserver.cpp#L977)） | 方案的 hook 改造对两条路径都生效，顺带统一、删掉冗余的 `killMonitorTask`。 |

---

## 3. 方案概述

**根治点 =「休眠/退出判断由 LINE hook 接管」，Lua 跑在哪个任务是次要的。**

保留 Lua 专用任务（8192 栈规避爆栈），但把「无操作超时 + 中键长按 >1s 退出」直接做进 hook 和各 yield 点，**Lua 任务自己停自己**，再通知 `setup()` 返回。控制流单线程化：

- **Lua 运行期**：主任务挂起在 `ulTaskNotifyTake`（不碰 display）；只有 Lua 任务在动，display 单线程访问，**无竞态**。
- **Lua 停后**：任务 `closeLua` → `xTaskNotifyGive` → `vTaskDelete`（自杀，**无泄漏**）→ 主任务 `setup()` 返回 → 独占保持期/深睡。

> 用户已确认产品决策：**中键长按 >1 秒**作为系统保留「退出当前 Lua App」手势（脚本 `tryGetKey` 拿不到中键长按，短按 <1s 仍可用）。

### 3.1 核心检测逻辑 `luaSysTick()`

由 LINE hook + `delay`/`waitKey`/`waitButton`/`tryGetKey` 的轮询点统一调用：

```c
// 活动刷新：任意键按下 = 用户在操作（digitalRead 反映物理电平，不消费事件队列）
if (digitalRead(L)==LOW || digitalRead(C)==LOW || digitalRead(R)==LOW)
    g_lastActivityMs = millis();

// (1) 无操作超时 → 深睡（Web IDE 模式 g_luaSuppressSleep 时跳过）
if (!g_luaSuppressSleep && millis() - g_lastActivityMs > g_sleepToSec*1000) {
    g_luaStopReason = SLEEP;  g_luaStopRequested = true;  luaL_error(L, "sleep timeout");
}
// (2) 中键长按 >1s → 退出（边沿计时；短按 <1s 松开后 g_cTiming 重置，事件正常进队列给脚本）
if (digitalRead(C)==LOW) {
    if (!g_cTiming) { g_cTiming = true; g_cStart = millis(); }
    else if (millis() - g_cStart >= 1000) {
        g_luaStopReason = EXIT;  g_luaStopRequested = true;  luaL_error(L, "exit by user");
    }
} else g_cTiming = false;
```

**为什么用「边沿计时」而非「空闲时间」**：`digitalRead` 反映物理电平，不受脚本 `tryGetKey` 消费事件影响。用户按住中键 1.2s → 物理 LOW 持续 1.2s → 1s 时退出；短按 200ms → 松开 HIGH → `g_cTiming` 重置，不退出，中键事件正常给脚本。短按/长按天然区分。

**退出路由**（`setup()` 返回后据 `luaSysStopReason()`）：

| 退出原因 | `setup()` 行为 | 后续 |
|----------|----------------|------|
| `EXIT`（中键长按）或 `NONE`（脚本自然结束） | `appManager.requestSelector()` | `run()` 进 App 列表 |
| `SLEEP`（无操作超时） | 直接返回 | `run()` 走保持期 → `deepSleep()` |

---

## 4. 实现步骤（有序，每步可独立验证）

> 状态：✅ 代码完成（7 步落地，待 `idf.py build` + 烧录验证）

### 步骤 1 — GUI 层加停止检查钩子 ✅（堵 menu/msgbox 盲区，致命问题 C）

[`components/starboard_gui/starboard_gui.cpp`](../components/starboard_gui/starboard_gui.cpp) + 头文件：

- 新增 `GUI::setStopCheck(bool(*)())` / `GUI::resetStopCheck()`（默认 `nullptr`）。回调**只检查中键长按/超时条件、置 `g_luaStopRequested=true`、返回 bool，不 `luaL_error`**——GUI 层无 `lua_State`，且要保证 C++ 栈正常展开，避免 `menu` 里 `new` 的内存因 longjmp 泄漏。
- `waitKeyEvent` 循环顶部：`if (s_stopCheck && s_stopCheck()) return -2;`（哨兵）。
- `menu`/`msgbox`/`msgbox_yn`/`msgbox_number`/`waitLongPress` 识别哨兵 `-2` → 提前**正常 return**（取消/默认值），保证 C++ 局部对象析构、堆释放。

### 步骤 2 — Lua 监控状态 + `luaSysTick` ✅（[`starboard_lua.cpp`](../components/lua/starboard_lua.cpp) + [.h](../components/lua/include/starboard_lua.h)）

- 新增状态：`g_lastActivityMs`、`g_luaStopReason`(NONE/SLEEP/EXIT)、`g_luaSuppressSleep`、`g_sleepToSec`、中键计时 `g_cTiming`/`g_cStart`。
- `luaSysBeginRun(bool suppressSleep)`：读 `hal.pref("sleep_to")`（最小 10）、重置 reason/activity/stopFlag/cTiming、设 suppressSleep。
- `luaSysEndRun()`：清理。
- `luaSysTick(lua_State* L)`：第 3.1 节核心检测；命中设 reason + `g_luaStopRequested` + `luaL_error`。
- `luaSysStopReason()`：供 `setup()` 返回后查询。
- 扩展 `luaStopHook`：在现有 requestLuaStop/喂狗之外调 `luaSysTick(L)`。
- `common_delay`、`gui_waitKey`/`waitButton`/`tryGetKey`（[`lua_gui.cpp`](../components/lua/lua_gui.cpp)）轮询步里调 `luaSysTick(L)`（堵 C 函数阻塞期间 hook 不触发的盲区）。

### 步骤 3 — 重写 `LuaApp` ✅（[`lua_app_wrapper.cpp`](../components/lua/lua_app_wrapper.cpp)，致命问题 B）

- `luaAppTask`：**删除任务内的 `appManager.goBack()`**（[L97-103](../components/lua/lua_app_wrapper.cpp#L97)）。改为：
  ```
  openLua → luaSetCurrentApp → luaSysBeginRun(false) → lua_execute → luaSysEndRun
         → closeLua → g_luaRunning=false → xTaskNotifyGive(notifyTask) → vTaskDelete(NULL)
  ```
  任务退出后**绝不碰 display/appManager**。
- `LuaApp::setup()`：仍 `xTaskCreate`(`LUA_TASK_STACK=8192`)；`ulTaskNotifyTake` 改 **`portMAX_DELAY`**（去掉 30s 硬超时，因 hook 会真正停 Lua）；删除 `LuaTaskParam.timeoutFlag`/`timedOut` 整套。返回后据 `luaSysStopReason()` 路由（见 3.1 表）。

### 步骤 4 — `appManager` 新增 `pendingSelector` ✅（[`starboard_app.cpp`](../components/starboard_app/starboard_app.cpp) + [.h](../components/starboard_app/include/starboard_app.h)，致命问题 A）

- 新增成员 `bool pendingSelector` + `void requestSelector() { pendingSelector = true; }`。
- `run()` 在 step3 的 do-while **之后、step4（记 lastAppName）之前**插入：
  ```cpp
  if (pendingSelector) {
      pendingSelector = false;
      openSelector();              // 内部 GUI::menu 阻塞；选中设 pendingSwitch
      if (pendingSwitch || pendingBack) goto step3;  // 链式切换，同现有 L270 模式
  }
  ```

### 步骤 5 — lua_gui 绑定补 `luaL_error` ✅（配合步骤 1 哨兵）

[`lua_gui.cpp`](../components/lua/lua_gui.cpp) 的 `gui_menu`/`msgbox`/`msgbox_yn`/`msgbox_number`：调用对应 GUI 函数返回后，加 `if (luaStopRequested()) luaL_error(L, "stopped");`（此时 C++ 栈已正常展开、内存已释放，`luaL_error` 在 Lua 绑定层 L 上下文安全 longjmp 回 `lua_execute`）。

### 步骤 6 — Web IDE 适配 ✅（[`lua_webserver.cpp`](../components/lua/lua_webserver.cpp)）

- `pollRunRequest`：`lua_execute` 前 `luaSysBeginRun(true)`（**豁免超时深睡**，连着电脑保持唤醒；保留中键 >1s 退出 Lua 回 IDE），后 `luaSysEndRun()`。
- 删除 `killMonitorTask` 及 `startBlocklyServer` 里的 `xTaskCreate(killMonitorTask...)`（hook 取代）。
- `appWebIDE` 自己的中键短按退出（[`appWebIDE.cpp:100`](../main/apps/appWebIDE.cpp#L100)）和 10 分钟空闲超时（[L92](../main/apps/appWebIDE.cpp#L92)）保留不动。

### 步骤 7 — 收尾 ✅

- [`lua_http.cpp`](../components/lua/lua_http.cpp)：`cfg.timeout_ms` 10s → 3s（收窄盲区）。
- [`lua_sys.cpp`](../components/lua/lua_sys.cpp)：`sys.yield` 改 no-op（hook 已自动 yield + 接管），更新注释（原注释「让 AppManager 检测睡眠计时器」是错的）。
- `lua_data` 模块已有按 App 名隔离的 NVS 持久化（`luaSetCurrentApp`），供脚本跨深睡存状态。

---

## 5. 关键文件

| 文件 | 改动 | 步骤 |
|------|------|------|
| [`components/starboard_gui/starboard_gui.cpp`](../components/starboard_gui/starboard_gui.cpp) + .h | `setStopCheck`/`waitKeyEvent` 哨兵、menu/msgbox 识别哨兵 | 1 |
| [`components/lua/starboard_lua.cpp`](../components/lua/starboard_lua.cpp) + [.h](../components/lua/include/starboard_lua.h) | 监控状态、`luaSysTick`、hook 扩展、begin/end | 2 |
| [`components/lua/lua_app_wrapper.cpp`](../components/lua/lua_app_wrapper.cpp) | 重写 `luaAppTask`（删 goBack）+ `LuaApp::setup`（同步路由、去 30s） | 3 |
| [`components/starboard_app/starboard_app.cpp`](../components/starboard_app/starboard_app.cpp) + [.h](../components/starboard_app/include/starboard_app.h) | `pendingSelector`/`requestSelector` + `run()` 连线 | 4 |
| [`components/lua/lua_gui.cpp`](../components/lua/lua_gui.cpp) | menu/msgbox 绑定补 `luaL_error`；waitKey 系列接 `luaSysTick` | 2, 5 |
| [`components/lua/lua_webserver.cpp`](../components/lua/lua_webserver.cpp) | `pollRunRequest` suppressSleep、删 killMonitorTask | 6 |
| [`components/lua/lua_http.cpp`](../components/lua/lua_http.cpp) / [`lua_sys.cpp`](../components/lua/lua_sys.cpp) | timeout 3s / yield no-op | 7 |

---

## 6. 风险与已知限制

| # | 项 | 处置 |
|---|----|------|
| 1 | **http.get 盲区** | `esp_http_client` 同步阻塞最长 3s（已从 10s 收窄），期间中键长按/超时检测失效，返回后恢复。可接受，本文记录。 |
| 2 | **display 全刷 ~5s 盲区**（可选） | `guiBusyCallback` 已 `vTaskDelay(1)` 喂狗且 `pollKeys` 不丢键；如需彻底，可在 callback 里加一次 `luaSysTick`（低优先）。 |
| 3 | **Lua App `resumable=true` 语义** | 深睡后 `lastAppName` 恢复该 App = **从头重跑 `main.lua`**（Lua 状态不跨深睡）。脚本要持久状态用 `data` 模块存 NVS。非回归（现状即如此），本文说明。 |
| 4 | **Web IDE `pollRunRequest` 仍在主任务（3584 栈）** | 现状，已验证简单脚本 OK。复杂脚本理论上有爆栈风险；如需统一稳健，后续可也改走专用任务（本轮不做，记录）。 |
| 5 | **中键长按 >1s 为系统手势** | 脚本 `tryGetKey` 拿不到中键长按（短按 <1s 仍返回 2）。Blockly 工具箱/文档需注明。 |
| 6 | **longjmp 跨 C++ 栈安全** | `luaL_error` 用 longjmp，不调 C++ 析构。故 GUI 停止检查用「返回哨兵 + Lua 绑定层 `luaL_error`」，保证 `menu` 的 `new` 内存先释放（步骤 1+5 配合）。 |
| 7 | **stopReason 跨任务可见性** | Lua 任务设、主任务读。`volatile` + `xTaskNotifyGive/Take` 的内存屏障（双向同步原语）已足够，无需 `std::atomic`。每次 `lua_execute` 入口重置 reason（避免上次残留）。 |

---

## 7. 验证（烧录后串口观察 `[LuaApp]`/`[Lua]` 诊断日志）

1. **死循环** `while true do end` → `sleep_to`（默认 60s）后日志 `stopReason=SLEEP` → 深睡 → 唤醒重跑。
2. **交互 App 挂着不操作**（`while true do k=gui.tryGetKey() ... end`）→ 60s 深睡；唤醒恢复重跑。
3. **运行中中键长按 >1s** → 日志 `stopReason=EXIT` → 进 App 列表（`openSelector`）。
4. **Lua 调 `gui.menu` 后挂着**（盲区修复验证）→ 60s 深睡（不卡死在 menu）。
5. **`delay(60000)` 期间中键长按** → 分段检查打断。
6. **短按中键** → 脚本 `tryGetKey` 正常收到 2，不误触退出。
7. **Web IDE 在线运行**（`/api/run`）→ 中键 >1s 停 Lua 回 IDE 界面；挂起**不**因超时深睡；`killMonitorTask` 已删仍能强停（hook 接管）。
8. **回归**：内置 App（Clock/Settings/OOBE）休眠、中键长按列表、保持期行为不变。

---

## 8. 进度日志

> 每次推进时在此追加一条。格式：`YYYY-MM-DD —— 内容`

- **2026-06-28 —— 方案制定完成**。诊断 Lua App 不能休眠的三大病灶（控制流割裂 / 30s 硬超时任务泄漏 / 停止机制未接系统侧），确认关键约束（主任务栈 3584 → 保留 Lua 专用任务；WDT 只监控 IDLE）。定方案 A：hook 接管休眠/退出 + 专用任务保留 + Lua 自停后通知 setup 返回。经 Plan agent 审查锁定三个致命点（任务内 goBack 删除、pendingSelector 连线、menu/msgbox 盲区必须本轮修）。用户确认中键长按 >1s 为系统退出手势。方案落本文档，**待排期实现**（步骤 1-7）。

- **2026-06-28 —— 代码实现完成（7 步全落地，待 `idf.py build` + 烧录验证）**。① `starboard_gui`：`setStopCheck`/`resetStopCheck` + `waitKeyEvent` 返回 `KEY_STOP` 哨兵 + menu/msgbox/msgbox_yn/msgbox_number/msgbox_time 识别哨兵提前返回（跳过 waitAllReleased、复位 pauseButtons）；② `starboard_lua`：`luaSysPollStop`/`luaSysTick`/`luaSysBeginRun`/`luaSysEndRun`/`luaSysStopReason`（活动刷新 + 无操作超时 + 中键长按>1s 边沿计时），hook/delay/waitKey/tryGetKey/sys.yield 全接入；③ `lua_app_wrapper`：重写 `luaAppTask`（删任务内 goBack）+ setup 据 `luaSysStopReason` 路由（EXIT/NONE→requestSelector，SLEEP→保持期深睡），去 30s 硬超时；④ `starboard_app`：`pendingSelector`/`requestSelector` + run() 连线（goto step3）；⑤ `lua_gui`：menu/msgbox 绑定补 `luaL_error`（内存释放后安全 longjmp）；⑥ `lua_webserver`：`pollRunRequest` 豁免超时深睡 + 删 killMonitorTask（hook 取代）；⑦ `lua_http` timeout 10s→3s。修了 C linkage 问题（`luaSysPollStopCpp` 包装 extern "C" 函数指针）、补 lua 组件 REQUIRES starboard_config。**待 `idf.py build` + 烧录验证 §7 的 8 项**。
