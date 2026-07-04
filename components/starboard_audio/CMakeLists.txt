# starboard_audio —— MAX98357A I2S 功放 + libhelix MP3 解码
#
# REQUIRES:
#   starboard_config         : PIN_BCLK/PIN_LRCLK/PIN_DIN
#   arduino-esp32            : Serial / delay
#   chmorgan__esp-libhelix-mp3: mp3dec.h(arduino-esp32 的 PRIVATE 传递依赖,必须显式 REQUIRES)
#   esp_driver_i2s           : driver/i2s_std.h(IDF 新版 I2S 驱动)
file(GLOB SRCS "starboard_audio.cpp" "*.cpp")
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES starboard_config arduino-esp32 chmorgan__esp-libhelix-mp3 esp_driver_i2s
)
