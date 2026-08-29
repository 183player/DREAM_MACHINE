# src/gui/CMakeLists.txt

find_package(Qt6 REQUIRED COMPONENTS Core Qml Quick Widgets)

add_executable(gui
        main.cpp
        plugin_loader.cpp
        plugin_loader.h
        session_state_manager.h
        status_provider.h
)

# 启用 MOC
set(CMAKE_AUTOMOC ON)

# 显式让 AUTOMOC 处理头文件（双重保险）
set_source_files_properties(
        session_state_manager.h
        status_provider.h
        PROPERTIES AUTOMOC ON
)

# 额外显式生成 moc 文件，确保链接（强烈推荐）
qt6_wrap_cpp(moc_sources
        session_state_manager.h
        status_provider.h
)
target_sources(gui PRIVATE ${moc_sources})

# 链接库
target_link_libraries(gui PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Widgets
        dm_logger
        dm_base
        dm_pipe
)

# 包含目录
target_include_directories(gui PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/dm_base
        ${CMAKE_SOURCE_DIR}/platform/dm_pipe
        ${CMAKE_SOURCE_DIR}/platform/dm_logger
)