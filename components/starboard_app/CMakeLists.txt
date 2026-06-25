# starboard_app: 应用框架(AppBase + AppManager),参考 LiClock AppManager 改造为回合制。
# - starboard_hal: 深睡/唤醒原因/按键(run 系统手势、deepSleep)
# - starboard_gui: GUI::waitLongPress / GUI::menu / menu_item(openSelector)
# - starboard_display / starboard_config: 经下面两个传递可得(App 内部画帧用)
idf_component_register(
    SRCS "starboard_app.cpp"
    INCLUDE_DIRS "include"
    REQUIRES starboard_hal starboard_gui starboard_display starboard_config
)
