add_executable(executor main.cpp)

target_link_libraries(executor PRIVATE
        Qt6::Core
        dm_logger
        dm_base
        dm_pipe
        dm_event
        dm_signal      # A.8：显式 link
        # 注：A2 实现 RUN_SCRIPT 时需补 dm_process
)