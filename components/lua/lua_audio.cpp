// lua_audio —— audio 模块 Lua 绑定(MAX98357A I2S 功放)
//
// 暴露给 Lua:
//   audio.play(path)    异步播放 LittleFS 上的 MP3,立即返回 true/false(文件可打开即 true)
//   audio.beep(freq,ms) 同步提示音(阻塞到响完;freq 频率Hz,ms 时长,默认 200)
//   audio.volume(v)     设音量 0..21(软件缩放)
//   audio.stop()        停止当前播放
//   audio.playing()     是否正在播放 MP3(异步播放控制用)
//
// 典型用法(异步播放 + 任意键停):
//   audio.volume(15)
//   audio.play("bgm.mp3")
//   while audio.playing() do
//     if gui.tryGetKey() ~= 0 then audio.stop() end
//     sys.yield()
//   end
//
// 底层见 components/starboard_audio/(I2S driver/i2s_std + libhelix MP3)。

#include "starboard_lua.h"
#include "starboard_audio.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// audio.play(path):异步播放 MP3,立即返回。脚本用 audio.playing()/audio.stop() 控制。
static int audio_play(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, audio.playMp3(path));
    return 1;
}

// audio.beep(freq, ms):同步提示音(阻塞到响完)。freq Hz,ms 默认 200。
static int audio_beep(lua_State *L)
{
    uint32_t freq = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t ms = (uint32_t)luaL_optinteger(L, 2, 200);
    audio.tone(freq, ms);
    return 0;
}

// audio.volume(v):设音量 0..21。
static int audio_volume(lua_State *L)
{
    audio.setVolume((uint8_t)luaL_checkinteger(L, 1));
    return 0;
}

// audio.stop():停止当前播放。
static int audio_stop(lua_State *L)
{
    audio.stop();
    return 0;
}

// audio.playing():是否正在播放 MP3。
static int audio_playing(lua_State *L)
{
    lua_pushboolean(L, audio.isPlaying());
    return 1;
}

static const luaL_Reg _lualib[] = {
    {"play", audio_play},
    {"beep", audio_beep},
    {"volume", audio_volume},
    {"stop", audio_stop},
    {"playing", audio_playing},
    {NULL, NULL},
};

extern "C" int luaopen_audio(lua_State *L)
{
    luaL_newlib(L, _lualib);
    return 1;
}
