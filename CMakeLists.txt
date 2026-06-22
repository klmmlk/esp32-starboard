# ink_test 的 ESP-IDF 工程根 CMake
# 使用前请先在「ESP-IDF 命令行环境」里打开本目录,确认 IDF_PATH 环境变量已设置。
cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 的构建系统(必须通过 IDF_PATH 定位)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ink_test_idf)
