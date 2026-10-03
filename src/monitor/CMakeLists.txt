add_executable(monitor main.cpp)

target_link_libraries(monitor PRIVATE
        Qt6::Core
        dm_logger
        dm_base
        dm_pipe
        dm_process
        dm_event
        dm_signal      # A.8：显式 link（dm_logger 依赖为 PRIVATE，不传递）
)