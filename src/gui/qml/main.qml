// src/gui/qml/main.qml
// 占位窗口（fallback），仅在加载超时或出错时显示
// 正常快速加载时保持隐藏，避免闪烁

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 360
    height: 150
    visible: false                     // 默认不可见，由 C++ 控制显示
    title: "Dream Machine"
    color: "#F0F0F0"

    // C++ 注入的状态对象
    property var statusProvider

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12
        width: parent.width - 40

        // 加载动画：三个竖条（透明度循环）
        Row {
            id: loadingRow
            Layout.alignment: Qt.AlignHCenter
            spacing: 6
            visible: statusProvider ? statusProvider.loading : true

            Repeater {
                model: 3
                Rectangle {
                    id: bar
                    width: 8
                    height: 28
                    radius: 2
                    color: "#333333"
                    opacity: 0.2

                    SequentialAnimation {
                        loops: Animation.Infinite
                        running: statusProvider ? statusProvider.loading : true
                        PauseAnimation { duration: index * 150 }
                        NumberAnimation {
                            target: bar
                            property: "opacity"
                            to: 1.0
                            duration: 400
                            easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            target: bar
                            property: "opacity"
                            to: 0.2
                            duration: 400
                            easing.type: Easing.InOutQuad
                        }
                    }
                }
            }
        }

        // 状态文本
        Text {
            id: statusText
            Layout.alignment: Qt.AlignHCenter
            text: statusProvider ? statusProvider.statusText : "正在加载插件..."
            color: "#333333"
            font.pointSize: 10
            font.family: "Segoe UI"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        // 错误信息
        Text {
            id: errorText
            Layout.alignment: Qt.AlignHCenter
            text: statusProvider ? statusProvider.errorText : ""
            color: "#D32F2F"
            font.pointSize: 9
            font.family: "Segoe UI"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
            visible: text.length > 0
        }

        // 退出按钮
        Button {
            id: exitButton
            Layout.alignment: Qt.AlignHCenter
            text: "退出"
            visible: statusProvider ? statusProvider.showExitButton : false
            onClicked: {
                if (statusProvider && statusProvider.exitApp) {
                    statusProvider.exitApp()
                }
            }
        }
    }
}