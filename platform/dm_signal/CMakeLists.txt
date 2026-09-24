# platform/dm_signal/CMakeLists.txt
add_library(dm_signal STATIC
        signal_bus.cpp
        signal_strings.cpp
        operation_tracker.cpp
        message_bridge.cpp

        signal_types.h
        signal_sink.h
        signal_bus.h
        signal_macros.h
        signal_strings.h
        operation_tracker.h
        message_bridge.h
)

target_include_directories(dm_signal PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(dm_signal PUBLIC cxx_std_17)