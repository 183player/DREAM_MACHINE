add_executable(gui main.cpp)

# 启用 MOC（元对象编译器）
set(CMAKE_AUTOMOC ON)

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

# 额外显式包含目录（确保 Clangd 和编译器都能找到）
target_include_directories(gui PRIVATE
        ${Qt6Core_INCLUDE_DIRS}
        ${Qt6Gui_INCLUDE_DIRS}
        ${Qt6Qml_INCLUDE_DIRS}
        ${Qt6Quick_INCLUDE_DIRS}
)