// src/default_plugin/qml/components/SearchButton.qml
// 搜索按钮（系统插件）
// 可被用户插件覆盖以自定义搜索行为
// 标准接口：searchRequested(text) 和 searchCancelled()

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    // ---------- 标准接口属性 ----------
    property bool visible: true
    property string placeholder: "搜索..."
    property var onSearchRequested: function(text) {}   // 搜索请求信号
    property var onSearchCancelled: function() {}       // 搜索取消信号

    // ---------- 状态 ----------
    property bool expanded: false

    // ---------- 内部函数（定义在使用之前） ----------
    function expandSearch() {
        root.expanded = true
        searchInput.forceActiveFocus()
    }

    function cancelSearch() {
        root.expanded = false
        searchInput.text = ""
        if (root.onSearchCancelled) {
            root.onSearchCancelled()
        }
    }

    function doSearch() {
        // 使用 const（QML 中 var 是标准写法，但明确语义使用 const 更清晰）
        // 注意：QML 中 const 可用，但某些旧版本可能不支持，使用 var 更兼容
        var text = searchInput.text.trim()
        if (text.length === 0) {
            cancelSearch()
            return
        }
        if (root.onSearchRequested) {
            root.onSearchRequested(text)
        }
        // 搜索后保持展开状态，显示结果
    }

    // ---------- 布局 ----------
    RowLayout {
        anchors.fill: parent
        spacing: 4

        // 搜索框（展开时显示）
        Rectangle {
            id: searchFieldContainer
            Layout.fillWidth: root.expanded
            Layout.preferredWidth: root.expanded ? 0 : 0
            Layout.minimumWidth: 0
            height: parent.height
            color: root.g ? root.g.colors.background : "#1E1E2E"
            radius: root.g ? root.g.spacing.border_radius : 4
            border.color: root.g ? root.g.colors.border : "#3A3A5A"
            border.width: 1
            visible: root.expanded
            opacity: root.expanded ? 1 : 0

            Behavior on opacity {
                NumberAnimation { duration: root.g ? root.g.animation.duration_short : 150 }
            }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                TextField {
                    id: searchInput
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    placeholderText: root.placeholder
                    color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                    font.pointSize: root.g ? root.g.fonts.size_small : 10
                    background: Rectangle { color: "transparent" }

                    // Enter 触发搜索（使用 function(event) 明确参数，避免弃用警告）
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Enter || event.key === Qt.Key_Return) {
                            event.accepted = true
                            doSearch()
                        }
                        if (event.key === Qt.Key_Escape) {
                            event.accepted = true
                            cancelSearch()
                        }
                    }
                }

                Button {
                    text: "✕"
                    flat: true
                    width: 24
                    height: 24
                    padding: 0
                    onClicked: cancelSearch()
                }
            }
        }

        // 搜索图标按钮
        Button {
            id: toggleButton
            text: "🔍"
            flat: true
            Layout.preferredWidth: 36
            Layout.fillHeight: true

            onClicked: {
                if (root.expanded) {
                    if (searchInput.text.trim().length > 0) {
                        doSearch()
                    } else {
                        cancelSearch()
                    }
                } else {
                    expandSearch()
                }
            }
        }
    }

    // ---------- 将全局参数传递到内部 ----------
    property var g: globalParams || null
}