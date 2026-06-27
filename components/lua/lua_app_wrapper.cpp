// lua_app_wrapper —— 扫描 /littlefs/apps/,包装为 AppBase
//
// 方案B: LuaApp::setup() 不阻塞,创建后台 FreeRTOS 任务跑 Lua。
// Lua 任务结束后通知主任务,然后 appManager.goBack() 切回上一个 App。

#include "lua_app_wrapper.h"
#include <starboard_app.h>
#include <starboard_lua.h>
#include <Arduino.h>
#include <dirent.h>
#include <string.h>
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
    bool *timeoutFlag;        // 指向调用方的超时标志,超时后置位防止 goBack 竞态
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
// 跑完 lua_execute 后通知主任务,然后 appManager.goBack() 切回上一个 App
static void luaAppTask(void *param)
{
    LuaTaskParam *p = (LuaTaskParam *)param;
    const char *luaPath = p->luaPath;
    TaskHandle_t notifyTask = p->notifyTask;

    Serial.printf("[LuaApp] 任务开始: %s\n", p->appName);
    lua_State *L = openLua();
    if (!L)
    {
        Serial.printf("[LuaApp] openLua 失败\n");
        // 通知主 setup() 等待结束,自己进 goBack
        if (!p->timeoutFlag || !(*p->timeoutFlag))
            appManager.goBack();
        xTaskNotifyGive(notifyTask);
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("[LuaApp] 执行 %s\n", luaPath);
    luaSetCurrentApp(p->appName);
    Serial.printf("[LuaApp] lua_execute 开始\n");
    int ret = lua_execute(L, luaPath);
    Serial.printf("[LuaApp] lua_execute 返回 ret=%d\n", ret);

    if (ret != 0)
    {
        Serial.printf("[LuaApp] %s: 执行错误\n", p->appName);
    }

    Serial.printf("[LuaApp] 关闭 Lua 状态机\n");
    closeLua(L);

    // Lua 执行完毕:通知等待中的 setup(),然后切换回上一个 App
    // 注意:如果 setup() 已超时(timedOut=true),不再调 goBack(),避免竞态
    if (!p->timeoutFlag || !(*p->timeoutFlag))
    {
        Serial.printf("[LuaApp] 正常结束,goBack\n");
        appManager.goBack();
    }
    else
    {
        Serial.printf("[LuaApp] 已超时,跳过 goBack\n");
    }
    xTaskNotifyGive(notifyTask);

    // 任务自我删除
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

        // 超时标志:setup() 超时后置位,luaAppTask 看到后不再调 goBack()
        bool timedOut = false;

        // 准备任务参数
        LuaTaskParam *param = new LuaTaskParam();
        strncpy(param->appName, name, sizeof(param->appName) - 1);
        param->appName[sizeof(param->appName) - 1] = '\0';
        strncpy(param->luaPath, luaPath, sizeof(param->luaPath) - 1);
        param->luaPath[sizeof(param->luaPath) - 1] = '\0';
        param->notifyTask = xTaskGetCurrentTaskHandle();
        param->timeoutFlag = &timedOut;

        // 创建后台任务跑 Lua(独立栈,不阻塞 AppManager 主循环)
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
            appManager.goBack();
            return;
        }

        // 等待 Lua 任务执行完毕(最多 30 秒超时,防止 Lua 脚本卡死导致永不休眠)
        // 通知来自 luaAppTask 内部 xTaskNotifyGive
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        if (!notified)
        {
            Serial.printf("[LuaApp] %s: Lua 任务超时(30s),强制返回\n", name);
            timedOut = true; // 通知 luaAppTask 不要调 goBack
            // 任务可能还活着,但 setup() 必须返回让系统能休眠
        }
        else
        {
            Serial.printf("[LuaApp] %s: Lua 任务完成\n", name);
        }
    }

	private:
    // name/title 指向堆内存,在本类构造时分配、析构时释放。
    // 基类声明为 const char* 故不能被赋值,用 const_cast 绕开直接初始化的限制。
    // 更好的做法是存 char[] 成员然后让基类指针指向它,但这种写法对于简易实现足够。
};

// 存储扫描到的 LuaApp 实例(防止析构)
std::vector<LuaApp *> g_luaApps;

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
        // 跳过 . 和 ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        // 只认目录
        if (ent->d_type != DT_DIR)
            continue;

        // 检查 conf.lua 是否存在
        char confPath[128];
        snprintf(confPath, sizeof(confPath), "%s/%s/conf.lua", APPS_DIR, ent->d_name);

        char *confContent = readFile(confPath);
        if (!confContent)
        {
            Serial.printf("[LuaApp] %s: 无 conf.lua,跳过\n", ent->d_name);
            continue;
        }

        // 简易解析:从 conf.lua 中提取 title 变量
        // conf.lua 示例: title = "我的应用"
        const char *titleStr = "未命名";

        // 在文本中查找 title = "xxx"
        const char *tpos = strstr(confContent, "title");
        if (tpos)
        {
            const char *q1 = strchr(tpos, '\"');
            if (q1)
            {
                const char *q2 = strchr(q1 + 1, '\"');
                if (q2)
                {
                    // 提取标题到临时缓冲区
                    static char titleBuf[64];
                    size_t tlen = (size_t)(q2 - q1 - 1);
                    if (tlen > 63) tlen = 63;
                    memcpy(titleBuf, q1 + 1, tlen);
                    titleBuf[tlen] = '\0';
                    titleStr = titleBuf;
                }
            }
        }
        free(confContent);

        // 创建并注册 App
        LuaApp *app = new LuaApp(ent->d_name, titleStr);
        g_luaApps.push_back(app);
        appManager.registerApp(app);

        Serial.printf("[LuaApp] 注册: %s → \"%s\"\n", ent->d_name, titleStr);
    }

    closedir(dir);
    Serial.printf("[LuaApp] 扫描完成,共注册 %u 个 App\n", (unsigned)g_luaApps.size());
}
