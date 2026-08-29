cmake_minimum_required(VERSION 3.20)
project(DreamMachine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置可执行文件输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 查找 Qt6（仅基础 Core，用于 JSON 和字符串）
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
message(STATUS "Qt6 found at: ${Qt6_DIR}")

# 添加子目录（基础设施先编译）
add_subdirectory(platform)

# 各进程
add_subdirectory(src/launcher)
add_subdirectory(src/monitor)
add_subdirectory(src/executor)
add_subdirectory(src/gui)
add_subdirectory(src/core_engine)

# ============================================================
# 默认系统插件（必须放在所有进程目标之后，因为需要依赖它们）
# ============================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/src/default_plugin/manifest.json)
    add_subdirectory(src/default_plugin)
else()
    message(WARNING "Default plugin manifest not found: ${CMAKE_SOURCE_DIR}/src/default_plugin/manifest.json")
endif()
