# platform/dm_base/CMakeLists.txt
add_library(dm_base STATIC
        constants.cpp
        plugin_types.h
        plugin_types.cpp
        messages.h
        messages.cpp
        common_utils.h
)

target_include_directories(dm_base PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dm_base PUBLIC
        Qt6::Core
)