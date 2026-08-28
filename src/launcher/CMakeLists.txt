add_executable(launcher main.cpp)

target_link_libraries(launcher PRIVATE
        Qt6::Core
        dm_logger
        dm_base      # 新增：提供 constants.h 等
        dm_pipe      # 新增：提供 NamedPipe
        dm_process   # 新增：提供 Process/ProcessStartOptions
)