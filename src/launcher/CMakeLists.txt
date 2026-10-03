add_executable(launcher main.cpp
        plugin_manager.h
        plugin_manager.cpp)

target_link_libraries(launcher PRIVATE
        Qt6::Core
        dm_logger
        dm_base
        dm_pipe
        dm_process
        dm_event
        dm_signal      # A.8：dm_logger 的 dm_signal 依赖为 PRIVATE，不传递；
        #      各进程必须显式 link
)