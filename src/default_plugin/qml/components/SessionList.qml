// src/default_plugin/qml/components/SessionList.qml
// 会话列表组件
// 显示所有会话，每个条目右侧有 ⋮ 菜单（重命名/删除/陈列架）

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    // ---------- 属性 ----------
    property string currentSessionId: ""          // 当前选中的会话 ID
    property var sessionModel: []                 // 会话列表数据（由 C++ 提供）
    property var onSessionSelected: function(sessionId) {}   // 会话选中回调
    property var onSessionMenuAction: function(action, sessionId) {} // 菜单动作回调

    // ---------- 布局 ----------
    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        // 标题
        Text {
            text: "会话"
            color: root.g ? root.g.colors.text_primary : "#EEEEEE"
            font.pointSize: root.g ? root.g.fonts.size_normal : 12
            font.weight: Font.Bold
            Layout.fillWidth: true
            Layout.margins: 4
        }

        // 会话列表（ListView）
        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2

            model: root.sessionModel
            delegate: sessionDelegate

            // 滚动条
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            // 空状态提示
            Text {
                anchors.centerIn: parent
                text: "暂无会话\n点击「新建」创建"
                color: root.g ? root.g.colors.text_muted : "#666666"
                font.pointSize: root.g ? root.g.fonts.size_small : 10
                horizontalAlignment: Text.AlignHCenter
                visible: listView.count === 0
            }
        }

        // 底部按钮区：新建会话 + 搜索
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 4
            spacing: 4

            // 新建会话按钮
            Button {
                text: "新建"
                Layout.fillWidth: true
                onClicked: {
                    // 触发新建会话逻辑（由父组件处理）
                    if (root.onSessionSelected) {
                        root.onSessionSelected("__new__")
                    }
                }
            }

            // 搜索按钮（占位，后续由 SearchButton 组件替换）
            Button {
                text: "🔍"
                Layout.preferredWidth: 40
                onClicked: {
                    // 触发搜索逻辑
                    console.log("Search clicked")
                }
            }
        }
    }

    // ---------- 会话条目委托 ----------
    Component {
        id: sessionDelegate

        Rectangle {
            id: delegateRoot
            width: listView.width
            height: 36
            color: {
                if (modelData.id === root.currentSessionId) {
                    return root.g ? root.g.colors.primary : "#3A7BD5"
                }
                return "transparent"
            }
            radius: 4

            RowLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                // 会话标题
                Text {
                    text: modelData.title || "未命名会话"
                    color: {
                        if (modelData.id === root.currentSessionId) {
                            return "#FFFFFF"
                        }
                        return root.g ? root.g.colors.text_primary : "#EEEEEE"
                    }
                    font.pointSize: root.g ? root.g.fonts.size_small : 10
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                // 菜单按钮（⋮）
                Button {
                    id: menuButton
                    text: "⋮"
                    flat: true
                    width: 24
                    height: 24
                    padding: 0
                    visible: modelData.id !== "__new__"

                    // 点击弹出菜单
                    onClicked: {
                        menu.popup()
                    }

                    // 菜单
                    Menu {
                        id: menu
                        x: parent.width - width
                        y: parent.height

                        MenuItem {
                            text: "重命名"
                            onTriggered: {
                                if (root.onSessionMenuAction) {
                                    root.onSessionMenuAction("rename", modelData.id)
                                }
                            }
                        }
                        MenuItem {
                            text: "删除"
                            onTriggered: {
                                if (root.onSessionMenuAction) {
                                    root.onSessionMenuAction("delete", modelData.id)
                                }
                            }
                        }
                        MenuItem {
                            text: "陈列架"
                            onTriggered: {
                                if (root.onSessionMenuAction) {
                                    root.onSessionMenuAction("shelf", modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            // 点击条目选中会话
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    if (root.onSessionSelected) {
                        root.onSessionSelected(modelData.id)
                    }
                }
            }
        }
    }

    // ---------- 将全局参数传递到内部（由 main.qml 注入） ----------
    property var g: globalParams || null

    // 监听外部传入的 g 对象变化
    onGChanged: {
        // 可在此触发刷新
    }
}