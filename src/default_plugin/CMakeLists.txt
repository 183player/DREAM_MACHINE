# src/default_plugin/CMakeLists.txt
# 默认系统插件构建配置
# 将插件源文件复制到构建目录的 plugins/system/ 和 data/backup/system_default/

set(DEFAULT_PLUGIN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SYSTEM_PLUGIN_DEST_DIR ${CMAKE_BINARY_DIR}/plugins/system/dream_machine_default)
set(BACKUP_PLUGIN_DEST_DIR ${CMAKE_BINARY_DIR}/data/backup/system_default)

# 创建自定义目标：复制默认插件
add_custom_target(copy_default_plugin ALL
        # 先删除目标目录（确保干净复制）
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${SYSTEM_PLUGIN_DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${BACKUP_PLUGIN_DEST_DIR}
        # 复制到运行时插件目录
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEFAULT_PLUGIN_SRC_DIR} ${SYSTEM_PLUGIN_DEST_DIR}
        # 复制到备份目录（用于完整性恢复）
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEFAULT_PLUGIN_SRC_DIR} ${BACKUP_PLUGIN_DEST_DIR}
        # 依赖 manifest.json，变更时重新复制
        DEPENDS ${DEFAULT_PLUGIN_SRC_DIR}/manifest.json
        COMMENT "Copying default system plugin to build directory (runtime + backup)"
        VERBATIM
)

# 让 launcher 依赖此目标，确保插件在 launcher 构建前就位
add_dependencies(launcher copy_default_plugin)

# 可选：同时让其他可能使用插件的目标也依赖
if(TARGET gui)
    add_dependencies(gui copy_default_plugin)
endif()
if(TARGET executor)
    add_dependencies(executor copy_default_plugin)
endif()
if(TARGET monitor)
    add_dependencies(monitor copy_default_plugin)
endif()

# 将默认插件源文件目录设置为只读属性（cmake 层面提示，实际文件属性由 PluginManager 设置）
# 这里只做标记，实际属性设置由代码完成

message(STATUS "Default system plugin: ${DEFAULT_PLUGIN_SRC_DIR}")
message(STATUS "  -> runtime: ${SYSTEM_PLUGIN_DEST_DIR}")
message(STATUS "  -> backup: ${BACKUP_PLUGIN_DEST_DIR}")