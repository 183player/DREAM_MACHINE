add_executable(executor main.cpp)

target_link_libraries(executor PRIVATE
        Qt6::Core
        dm_logger
        dm_base
        dm_pipe
        dm_event
)