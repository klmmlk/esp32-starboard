// lua_app_wrapper —— 扫描 /littlefs/apps/,包装为 AppBase
//
// 阶段5b:简易版,不引入 LuaAppWrapper 完整生命周期(lightsleep/wakeup等),
//         只有 setup() 执行一次 main.lua,返回后 goBack。
//
// LiClock 完整生命周期(conf.lua + main.lua + lightsleep/wakeup/deepsleep)
// 待阶段5c 补全。

#include "lua_app_wrapper.h"
#include <starboard_app.h>
#include <starboard_lua.h>
#include <Arduino.h>
#include <dirent.h>
#include <string.h>
#include <vector>

namespace
{

constexpr const char *APPS_DIR = "/littlefs/apps";

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
        char path[128];
        snprintf(path, sizeof(path), "%s/%s/main.lua", APPS_DIR,
                 name ? name : "");

        // 打开 Lua 并执行 main.lua
        lua_State *L = openLua();
        if (!L)
        {
            Serial.printf("[LuaApp] %s: openLua 失败\n", name);
            appManager.goBack();
            return;
        }

        Serial.printf("[LuaApp] 执行 %s\n", path);
        luaSetCurrentApp(name); // 供 data.save/load 按 App 隔离
        int ret = lua_execute(L, path);

        if (ret != 0)
        {
            Serial.printf("[LuaApp] %s: 执行错误\n", name);
        }

        closeLua(L);
        appManager.goBack();
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
