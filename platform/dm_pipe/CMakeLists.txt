add_library(dm_pipe STATIC
        pipe.h
        pipe.cpp
)

target_include_directories(dm_pipe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(dm_pipe PRIVATE
        dm_logger
        dm_base
)