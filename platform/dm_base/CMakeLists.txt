# dm_base - 纯头文件库（常量和工具函数）
add_library(dm_base INTERFACE)

target_include_directories(dm_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖 Qt6::Core（QJsonDocument 需要）
target_link_libraries(dm_base INTERFACE Qt6::Core)