// src/default_plugin/qml/cards/TracePopup.qml
// 追溯卡片弹窗（TracePopup）
// 显示 L1 完整片段（时间戳、锚点、角色、内容）
// 非模态窗口，右上角有关闭按钮（x）
// 用户可继续浏览对话

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    // ---------- 属性 ----------
    property string anchorId: ""              // 锚点 ID（如 #5A）
    property var traceData: []               // 片段数据 [{timestamp, role, content}]
    property var onClosed: function() {}     // 关闭回调

    // ---------- 窗口配置 ----------
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: 500
    height: Math.min(400, Math.max(200, traceData.length * 32 + 80))
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    padding: 0

    // ---------- 背景 ----------
    background: Rectangle {
        color: root.g ? root.g.colors.background : "#1E1E2E"
        radius: root.g ? root.g.spacing.border_radius + 2 : 6
        border.color: root.g ? root.g.colors.border : "#3A3A5A"
        border.width: 1

        // 阴影效果
        layer.enabled: true
        layer.effect: DropShadow {
            horizontalOffset: 4
            verticalOffset: 4
            radius: 8
            color: "#80000000"
            samples: 16
        }
    }

    // ---------- 内容布局 ----------
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // ----- 标题栏（锚点 ID + 关闭按钮） -----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "追溯原始片段 (anchor: " + (root.anchorId || "未知") + ")"
                color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                font.pointSize: root.g ? root.g.fonts.size_normal : 12
                font.weight: Font.Bold
                Layout.fillWidth: true
            }

            Button {
                text: "✕"
                flat: true
                width: 28
                height: 28
                padding: 0
                onClicked: {
                    root.close()
                    if (root.onClosed) {
                        root.onClosed()
                    }
                }

                contentItem: Text {
                    text: parent.text
                    color: root.g ? root.g.colors.text_secondary : "#AAAAAA"
                    font.pointSize: root.g ? root.g.fonts.size_normal : 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // ----- 分隔线 -----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: root.g ? root.g.colors.border : "#3A3A5A"
        }

        // ----- 片段列表 -----
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            Column {
                width: parent.width - 8
                spacing: 4

                Repeater {
                    model: root.traceData

                    Rectangle {
                        width: parent.width
                        height: entryText.height + 8
                        color: (index % 2 === 0) ?
                            (root.g ? root.g.colors.surface : "#2D2D44") :
                            "transparent"
                        radius: 2

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 8

                            // 时间戳
                            Text {
                                text: modelData.timestamp || "00:00:00"
                                color: root.g ? root.g.colors.text_muted : "#666666"
                                font.pointSize: root.g ? root.g.fonts.size_small : 9
                                Layout.preferredWidth: 80
                            }

                            // 角色标签
                            Text {
                                text: {
                                    if (modelData.role === "user") return "用户"
                                    if (modelData.role === "ai") return "AI"
                                    if (modelData.role === "system") return "系统"
                                    return modelData.role || "未知"
                                }
                                color: {
                                    if (modelData.role === "user") return "#81C784"
                                    if (modelData.role === "ai") return "#64B5F6"
                                    if (modelData.role === "system") return "#FFD54F"
                                    return root.g ? root.g.colors.text_secondary : "#AAAAAA"
                                }
                                font.pointSize: root.g ? root.g.fonts.size_small : 9
                                font.weight: Font.Medium
                                Layout.preferredWidth: 40
                            }

                            // 内容
                            Text {
                                id: entryText
                                text: modelData.content || ""
                                color: root.g ? root.g.colors.text_primary : "#EEEEEE"
                                font.pointSize: root.g ? root.g.fonts.size_small : 10
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }

        // ----- 底部关闭按钮 -----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                text: "关闭"
                Layout.preferredWidth: 80
                Layout.preferredHeight: 30
                onClicked: {
                    root.close()
                    if (root.onClosed) {
                        root.onClosed()
                    }
                }
            }
        }
    }

    // ---------- 将全局参数传递到内部 ----------
    property var g: globalParams || null

    // ---------- 便捷方法：从 L1 数据设置 -----
    function setTrace(anchor, lines) {
        root.anchorId = anchor || ""
        root.traceData = lines || []
    }

    // ---------- 便捷方法：添加单行片段 -----
    function addLine(timestamp, role, content) {
        var newData = root.traceData.slice()
        newData.push({
            timestamp: timestamp || "",
            role: role || "system",
            content: content || ""
        })
        root.traceData = newData
    }

    // ---------- 重置 -----
    function clear() {
        root.anchorId = ""
        root.traceData = []
    }
}