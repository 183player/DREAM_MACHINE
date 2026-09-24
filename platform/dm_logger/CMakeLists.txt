add_library(dm_logger STATIC
        logger.h
        logger.cpp)

target_include_directories(dm_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ================================================================
# 依赖：dm_signal
#
# PRIVATE 而非 PUBLIC —— 原因：
#   logger.h 不直接使用 dm_signal（不继承 ISignalSink）；
#   仅在 logger.cpp 内部用 Adapter（LoggerSignalAdapter）订阅信号。
#   因此无需传递 include 路径给下游。
#
# 与 dm_process / dm_pipe / dm_event 的 PRIVATE 风格一致。
# ================================================================
target_link_libraries(dm_logger PRIVATE dm_signal)