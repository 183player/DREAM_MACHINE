// src/default_plugin/qml/components/ButtonListContainer.qml
// 按钮列表容器（会话操作区）
// 用于显示操作按钮列表，支持排序、间距、标签显示
// 可被用户插件覆盖以调整布局

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "transparent"

    // ---------- 属性（可被覆盖） ----------
    property string order: "left_to_right"  // 枚举：left_to_right / right_to_left
    property int gap: 6                     // 间隔距离（像素）
    property bool showLabel: true           // 是否显示按钮标签
    property var buttonModel: []            // 按钮数据模型
    property var onButtonClicked: function(buttonId) {} // 按钮点击回调

    // ---------- 布局 ----------
    RowLayout {
        anchors.fill: parent
        spacing: root.gap
        layoutDirection: root.order === "right_to_left" ? Qt.RightToLeft : Qt.LeftToRight

        // 根据模型动态生成按钮
        Repeater {
            model: root.buttonModel
            delegate: Button {
                text: root.showLabel ? modelData.label : ""
                icon.source: modelData.icon || ""
                icon.width: 16
                icon.height: 16
                flat: true

                Layout.preferredWidth: root.showLabel ? implicitWidth : 40
                Layout.preferredHeight: 36

                onClicked: {
                    if (root.onButtonClicked) {
                        root.onButtonClicked(modelData.id || index)
                    }
                }

                // 提示文本
                ToolTip.visible: hovered && modelData.tooltip !== undefined
                ToolTip.text: modelData.tooltip || ""
                ToolTip.delay: 500
            }
        }
    }

    // ---------- 将全局参数传递到内部 ----------
    property var g: globalParams || null
}