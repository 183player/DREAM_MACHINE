// src/default_plugin/qml/loading.qml
// Dream Machine 引导 loading 窗口
//
// 用途：
//   GUI 启动 → 显示本窗口 → 等待 launcher 的 INIT_LIST → 加载框架 main.qml
//   框架加载成功后本窗口隐藏/销毁
//
// 与 main.qml 的区别：
//   - loading.qml：引导窗口（本文件），随系统插件部署
//   - main.qml   ：框架主窗口（REPLACE 目标），由插件提供
//
// 部署位置：
//   <exe_dir>/plugins/system/dream_machine_default/qml/loading.qml
//
// 主题与尺寸来自 globalParams（C++ 注入）：
//   - 颜色/字体：跟随当前主题模式（light / dark）
//   - 尺寸：基于屏幕分辨率动态计算（loading_width / loading_height）

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    // C++ 注入的全局参数（colors / fonts / layout / spacing / animation）
    readonly property var g: globalParams || null

    // 尺寸：基于屏幕分辨率动态计算（C++ 注入）
    // 兜底值 360×150 与 placeholder_*_base 一致
    width:  (g && g.layout) ? g.layout.placeholder_width  : 360
    height: (g && g.layout) ? g.layout.placeholder_height : 150

    visible: false                     // 默认不可见，由 C++ 控制显示
    title: "Dream Machine"
    color: (g && g.colors) ? g.colors.background : "#F0F0F0"

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
                    color: (g && g.colors) ? g.colors.text_primary : "#333333"
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
            color: (g && g.colors) ? g.colors.text_primary : "#333333"
            font.pointSize: (g && g.fonts) ? g.fonts.size_small : 10
            font.family: (g && g.fonts) ? g.fonts.family : "Segoe UI"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        // 错误信息
        Text {
            id: errorText
            Layout.alignment: Qt.AlignHCenter
            text: statusProvider ? statusProvider.errorText : ""
            color: (g && g.colors) ? g.colors.error : "#D32F2F"
            font.pointSize: (g && g.fonts) ? g.fonts.size_small : 9
            font.family: (g && g.fonts) ? g.fonts.family : "Segoe UI"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
            visible: text.length > 0
        }

        // 退出按钮
        // 保留 Qt 默认样式（跟随 QQuickStyle::setStyle("Fusion")）
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