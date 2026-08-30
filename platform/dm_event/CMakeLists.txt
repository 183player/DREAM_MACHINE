# platform/dm_event/CMakeLists.txt
add_library(dm_event STATIC
        event_loop.cpp
)

target_include_directories(dm_event PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 依赖日志模块（用于 LOG_* 宏）
target_link_libraries(dm_event PRIVATE
        dm_logger
)

# 显式包含日志模块头文件路径（确保编译时能找到 logger.h）
target_include_directories(dm_event PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/dm_logger
)

# 设置 C++ 标准（继承全局）
set_target_properties(dm_event PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
)