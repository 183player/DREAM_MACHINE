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
)