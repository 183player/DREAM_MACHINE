# src/core_engine/CMakeLists.txt
add_executable(core_engine main.cpp)

# 启用 MOC（如果需要 Qt 元对象，但当前没有 Qt 依赖）
# set(CMAKE_AUTOMOC ON)

# 包含路径：需要访问 platform 下的 dm_base 和 dm_pipe 头文件
target_include_directories(core_engine PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/dm_base
        ${CMAKE_SOURCE_DIR}/platform/dm_pipe
        ${CMAKE_SOURCE_DIR}/platform/dm_logger  # 如果需要 logger.h
        ${CMAKE_SOURCE_DIR}/platform/dm_event
)

# 链接库
target_link_libraries(core_engine PRIVATE
        dm_base
        dm_pipe
        dm_logger
        dm_event
        # 注意：dm_pipe 依赖 Windows 系统库，但无需额外指定
)