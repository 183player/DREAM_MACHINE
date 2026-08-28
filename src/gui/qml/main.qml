import QtQuick
import QtQuick.Window

Window {
    width: 800
    height: 600
    visible: true
    title: "Dream Machine GUI"

    Text {
        anchors.centerIn: parent
        text: "Dream Machine GUI\n(Connected to launcher)"
        horizontalAlignment: Text.AlignHCenter
        font.pointSize: 16
        color: "#333"
    }
}