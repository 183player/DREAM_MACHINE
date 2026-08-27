add_executable(core_engine main.cpp)
target_link_libraries(core_engine PRIVATE
        Qt6::Core
        dm_logger)