# platform/dm_pipe/CMakeLists.txt
add_library(dm_pipe STATIC
        pipe.cpp
)

# 公开头文件路径（供其他模块使用）
target_include_directories(dm_pipe PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 私有依赖：日志模块（pipe.cpp 中使用了 LOG_* 宏）
target_include_directories(dm_pipe PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/dm_logger
)

target_link_libraries(dm_pipe PRIVATE
        dm_logger
)

# Windows 特定：无需额外库，windows.h 为系统 API

# 设置 C++ 标准（继承全局设置即可）
# set_target_properties(dm_pipe PROPERTIES CXX_STANDARD 17)