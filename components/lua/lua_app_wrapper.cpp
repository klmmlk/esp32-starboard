// lua_app_wrapper —— 扫描 /littlefs/apps/,包装为 AppBase
//
// 方案B: LuaApp::setup() 不阻塞,创建后台 FreeRTOS 任务跑 Lua。
// Lua 任务结束后通知主任务,然后 appManager.goBack() 切回上一个 App。

#include "lua_app_wrapper.h"
#include <starboard_app.h>
#include <starboard_lua.h>
#include <starboard_config.h>  // PIN_BUTTONC(setup 退出时等中键松开)
#include <Arduino.h>
#include <dirent.h>
#include <string.h>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{

constexpr const char *APPS_DIR = "/littlefs/apps";

// Lua 任务栈大小(Lua 运行时需要足够的栈空间)
constexpr uint32_t LUA_TASK_STACK = 8192;

// Lua 后台任务参数
struct LuaTaskParam {
    char appName[64];
    char luaPath[128];
    TaskHandle_t notifyTask;  // 通知目标任务(即 AppManager.run 所在任务)
};

// 读取文件内容到堆(调用者 free)
static char *readFile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return nullptr;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0)
    {
        fclose(f);
        return nullptr;
    }

    char *buf = (char *)malloc(len + 1);
    if (!buf)
    {
        fclose(f);
        return nullptr;
    }

    size_t read = fread(buf, 1, len, f);
    fclose(f);
    buf[read] = '\0';
    return buf;
}

// Lua 后台任务函数
// 在独立任务(8192 栈,主任务仅 3582 不够)跑 lua_execute。LINE hook + 各 yield 点
// 接管「无操作超时→深睡 / 中键长按>1s→退出」:命中则 luaL_error,lua_execute 正常返回。
// 任务结束后仅 xTaskNotifyGive 通知主任务 setup() 继续——【绝不碰 display/appManager】,
// 路由(进列表/深睡)由主任务 setup 据 luaSysStopReason() 决定,保证单线程无竞态。
static void luaAppTask(void *param)
{
    LuaTaskParam *p = (LuaTaskParam *)param;
    TaskHandle_t notifyTask = p->notifyTask;

    Serial.printf("[LuaApp] 任务开始: %s\n", p->appName);
    lua_State *L = openLua();
    if (L)
    {
        luaSetCurrentApp(p->appName);
        luaSysBeginRun(false); // 注册系统监控(超时深睡 / 中键长按>1s 退出)
        Serial.printf("[LuaApp] lua_execute %s\n", p->luaPath);
        int ret = lua_execute(L, p->luaPath); // hook 内自停 → 返回非0
        Serial.printf("[LuaApp] lua_execute 返回 ret=%d reason=%d\n",
                      ret, (int)luaSysStopReason());
        luaSysEndRun();
        closeLua(L);
    }
    else
    {
        Serial.printf("[LuaApp] openLua 失败\n");
    }

    // 通知主任务 setup() 继续,然后自杀
    xTaskNotifyGive(notifyTask);
    vTaskDelete(NULL);
}

class LuaApp : public AppBase
{
	public:
    LuaApp(const char *dirName, const char *appTitle)
    {
        // 目录名即 App name(唯一标识)
        size_t dlen = strlen(dirName);
        name = new char[dlen + 1];
        strcpy((char *)name, dirName);

        // 标题来自 conf.lua(或在目录名基础上美化)
        size_t tlen = strlen(appTitle);
        title = new char[tlen + 1];
        strcpy((char *)title, appTitle);

        // Lua App 可恢复(resumable=true),从列表可见
        // (注:name/title 是 const char*,这里 cast 存堆字符串,生命周期与 App 同)
        const_cast<bool &>(showInList) = true;
        const_cast<bool &>(resumable) = true;
    }

    ~LuaApp() override
    {
        delete[] (char *)name;
        delete[] (char *)title;
    }

    void setup() override
    {
        // 构建 main.lua 路径
        char luaPath[128];
        snprintf(luaPath, sizeof(luaPath), "%s/%s/main.lua", APPS_DIR,
                 name ? name : "");

        // 准备任务参数
        LuaTaskParam *param = new LuaTaskParam();
        strncpy(param->appName, name, sizeof(param->appName) - 1);
        param->appName[sizeof(param->appName) - 1] = '\0';
        strncpy(param->luaPath, luaPath, sizeof(param->luaPath) - 1);
        param->luaPath[sizeof(param->luaPath) - 1] = '\0';
        param->notifyTask = xTaskGetCurrentTaskHandle();

        // 创建后台任务跑 Lua(独立 8192 栈,不阻塞 AppManager 主循环)
        TaskHandle_t luaTaskHandle = NULL;
        BaseType_t ok = xTaskCreate(
            luaAppTask,
            "LuaApp",
            LUA_TASK_STACK / sizeof(StackType_t),
            param,
            2,  // 优先级稍低于 main 任务
            &luaTaskHandle);

        if (ok != pdTRUE)
        {
            Serial.printf("[LuaApp] %s: 任务创建失败\n", name);
            delete param;
            appManager.requestSelector();
            return;
        }

        // 等 Lua 任务结束。hook 接管休眠/退出判断,任务一定会在
        // 「无操作超时 / 中键长按>1s / 脚本自然结束」后退出,故用 portMAX_DELAY(无需硬超时)。
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        Serial.printf("[LuaApp] %s: 任务结束 reason=%d\n", name, (int)luaSysStopReason());

        // 据退出原因路由(主任务做,任务本身不碰 appManager)
        if (luaSysStopReason() == LUA_STOP_SLEEP)
        {
            // 超时深睡:直接返回,run() 走保持期 → deepSleep
            Serial.printf("[LuaApp] %s: 超时 → 保持期深睡\n", name);
        }
        else
        {
            // EXIT(中键长按)或 NONE(脚本自然结束)。
            // 先等中键松开,避免 openSelector 的 menu 立刻收到残留中键事件
            while (digitalRead(PIN_BUTTONC) == HIGH) delay(10);
            delay(50);
            // 若脚本已主动 gotoApp/goBack(pending 已设),尊重脚本意图,不再 requestSelector
            // —— 否则 pendingSelector 残留,会在下个 App 的 setup 后误触发 openSelector(列表)
            //    而不是按 reason 走保持期/深睡。
            if (!appManager.hasPendingSwitch() && !appManager.hasPendingBack())
            {
                Serial.printf("[LuaApp] %s: 退出/结束 → App 列表\n", name);
                appManager.requestSelector();
            }
            else
            {
                Serial.printf("[LuaApp] %s: 结束(脚本已请求切换,不进列表)\n", name);
            }
        }
    }

	private:
    // name/title 指向堆内存,在本类构造时分配、析构时释放。
    // 基类声明为 const char* 故不能被赋值,用 const_cast 绕开直接初始化的限制。
    // 更好的做法是存 char[] 成员然后让基类指针指向它,但这种写法对于简易实现足够。
};

// 存储扫描到的 LuaApp 实例(防止析构)
std::vector<LuaApp *> g_luaApps;

// 从 conf.lua 内容解析 title(失败用"未命名"),写入 out。
static void parseAppTitle(const char *confContent, char *out, size_t outSize)
{
    strncpy(out, "未命名", outSize - 1);
    out[outSize - 1] = '\0';
    if (!confContent) return;
    const char *tpos = strstr(confContent, "title");
    if (!tpos) return;
    const char *q1 = strchr(tpos, '\"');
    if (!q1) return;
    const char *q2 = strchr(q1 + 1, '\"');
    if (!q2) return;
    size_t tlen = (size_t)(q2 - q1 - 1);
    if (tlen > outSize - 1) tlen = outSize - 1;
    memcpy(out, q1 + 1, tlen);
    out[tlen] = '\0';
}

// 注册单个 App 目录:已注册则跳过(增量),否则解析 conf.lua + new LuaApp + registerApp。
static bool registerLuaAppDir(const char *dirName)
{
    for (auto *a : g_luaApps)
        if (strcmp(a->name, dirName) == 0) return false; // 已注册,跳过
    char confPath[128];
    snprintf(confPath, sizeof(confPath), "%s/%s/conf.lua", APPS_DIR, dirName);
    char *confContent = readFile(confPath);
    if (!confContent) return false;
    char title[64];
    parseAppTitle(confContent, title, sizeof(title));
    free(confContent);
    LuaApp *app = new LuaApp(dirName, title);
    g_luaApps.push_back(app);
    appManager.registerApp(app);
    Serial.printf("[LuaApp] 注册: %s → \"%s\"\n", dirName, title);
    return true;
}

} // namespace

void scanAndRegisterLuaApps()
{
    DIR *dir = opendir(APPS_DIR);
    if (!dir)
    {
        Serial.printf("[LuaApp] 目录 %s 不存在,跳过\n", APPS_DIR);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_type != DT_DIR)
            continue;
        registerLuaAppDir(ent->d_name); // 增量:已注册的自动跳过
    }
    closedir(dir);
    Serial.printf("[LuaApp] 扫描完成,共注册 %u 个 App\n", (unsigned)g_luaApps.size());
}

// 增量同步 /littlefs/apps/ 与已注册 App(供 Web IDE 保存/删除后由【主线程】调用,
// 解决「Web IDE 新建 App 后应用列表找不到、需重启」的问题):
//   - 目录存在但未注册 → 注册(新增)
//   - 已注册但目录已删 → 注销并释放(删除)
void syncLuaApps()
{
    DIR *dir = opendir(APPS_DIR);
    if (!dir) return;
    std::vector<std::string> dirs;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_type != DT_DIR)
            continue;
        dirs.push_back(ent->d_name);
    }
    closedir(dir);

    // 注销已删除的(目录不存在的已注册 App)
    for (auto it = g_luaApps.begin(); it != g_luaApps.end(); )
    {
        bool exists = false;
        for (auto &d : dirs)
            if (strcmp((*it)->name, d.c_str()) == 0) { exists = true; break; }
        if (!exists)
        {
            Serial.printf("[LuaApp] 注销已删除: %s\n", (*it)->name);
            appManager.unregisterApp(*it);
            delete *it;
            it = g_luaApps.erase(it);
        }
        else ++it;
    }

    // 注册新增的
    for (auto &d : dirs)
        registerLuaAppDir(d.c_str());

    Serial.printf("[LuaApp] 同步完成,共 %u 个 App\n", (unsigned)g_luaApps.size());
}
