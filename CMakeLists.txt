# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ---- 固件版本（语义化版本 MAJOR.MINOR.PATCH）----
# MAJOR = 不兼容的行为改动（例：上电不再自动播电台）
# MINOR = 新增功能，向后兼容（例：蓝牙 open/close/free）
# PATCH = 修 bug，向后兼容
# 改代码后同步三处：这里的版本号 + CHANGELOG.md + git tag。
# ESP-IDF 会把 PROJECT_VER 作为编译宏传进去（固件启动时会打印出来）。
set(PROJECT_VER "1.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_max98357_fm)
