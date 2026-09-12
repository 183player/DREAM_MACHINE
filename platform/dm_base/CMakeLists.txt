# platform/dm_base/CMakeLists.txt

add_library(dm_base STATIC
        constants.h
        constants.cpp
        error_codes.h
        common_utils.h
        messages.h
        messages.cpp
        plugin_types.h
        plugin_types.cpp
        message_router.h
        init_list_utils.h
        init_list_utils.cpp
)

# 公开头文件路径（供其他模块使用）
target_include_directories(dm_base PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ================================================================
# 外部依赖：Qt（messages.cpp / plugin_types.cpp 使用 QJsonDocument）
# ================================================================
find_package(Qt6 REQUIRED COMPONENTS Core)

target_link_libraries(dm_base PUBLIC
        Qt6::Core
)

# ================================================================
# === 新增 ===
# 私有依赖：日志模块
#
# 依据 C5 依赖图：dm_base 依赖 dm_logger（单向，无循环）
# 使用场景：init_list_utils.cpp 中的 LOG_INFO / LOG_ERROR 宏
#
# 注：此依赖此前缺失（dm_base 从未编译过直接使用 LOG_* 的 .cpp），
#     init_list_utils.cpp 是首个在 dm_base 内部直接使用 LOG_* 的实现。
# ================================================================
target_include_directories(dm_base PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/dm_logger
)

target_link_libraries(dm_base PRIVATE
        dm_logger
)