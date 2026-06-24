# starboard_gui: 通用 GUI(消息框/菜单/数字·时间输入),移植自 LiClock GUI.cpp
# - starboard_display: display/u8g2/COL_*/CN_FONT_MAIN(经 PUBLIC 传 GxEPD2/U8g2/Adafruit_GFX/config)
# - starboard_hal: 按键轮询(hal.btn*.isPressing / pauseButtons)
idf_component_register(
    SRCS "starboard_gui.cpp"
    INCLUDE_DIRS "include"
    REQUIRES starboard_display starboard_hal
)
