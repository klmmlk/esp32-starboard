# starboard_config: 全局引脚定义、屏幕尺寸、常量
# 纯头文件组件,不编译任何 .cpp,供其它 component include
# ⚠️ 故意不 include display/hal/gui,避免循环依赖
idf_component_register(
    INCLUDE_DIRS "include"
)
