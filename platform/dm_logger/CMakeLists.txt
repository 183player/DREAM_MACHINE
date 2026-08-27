add_library(dm_logger STATIC
        logger.h
        logger.cpp)

target_include_directories(dm_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

