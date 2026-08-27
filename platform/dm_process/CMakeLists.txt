add_library(dm_process STATIC
        process.h
        process.cpp
)

target_include_directories(dm_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(dm_process PRIVATE
        dm_logger
        dm_base
)