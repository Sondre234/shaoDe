import QtQuick
import QtQuick.Controls.Basic

Rectangle {
    id: desktop
    color: shell.background
    Image {
        anchors.fill: parent
        source: shell.wallpaper
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
    }
    Rectangle {
        anchors.fill: parent
        visible: shell.wallpaper.toString().length === 0
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(shell.background, 1.45) }
            GradientStop { position: 1; color: shell.background }
        }
    }
    Text {
        anchors.right: parent.right; anchors.rightMargin: 40
        anchors.bottom: parent.bottom; anchors.bottomMargin: shell.panelHeight + 35
        text: "shaoDe"; font.pixelSize: 32; font.weight: Font.Light
        color: shell.textColor; opacity: 0.18
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function(mouse) {
            menu.visible = mouse.button === Qt.RightButton
            menu.x = Math.max(0, Math.min(mouse.x, desktop.width - menu.width - 8))
            menu.y = Math.max(0, Math.min(mouse.y, desktop.height - shell.panelHeight - menu.height - 8))
        }
    }
    Column {
        x: 16; y: 24; spacing: 12
        Repeater {
            model: shell.pinned
            delegate: Button {
                required property var modelData
                width: 88; height: 84
                onDoubleClicked: shell.launch(modelData.appId)
                ToolTip.visible: hovered; ToolTip.text: "Double-click to open " + modelData.name
                background: Rectangle { radius: 8; color: parent.hovered ? "#284b638a" : "transparent"; border.color: parent.hovered ? "#557da8ff" : "transparent" }
                contentItem: Column {
                    spacing: 6
                    Image { anchors.horizontalCenter: parent.horizontalCenter; width: 40; height: 40; sourceSize: Qt.size(40, 40); source: "image://icons/" + modelData.icon }
                    Text { width: parent.width; text: modelData.name; color: shell.textColor; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight }
                }
            }
        }
    }
    Rectangle {
        id: menu
        visible: false
        width: 210; height: 112; radius: 8
        color: shell.panelColor; border.color: Qt.lighter(shell.panelColor, 1.6)
        Column {
            anchors.fill: parent; anchors.margins: 6
            Button { width: parent.width; height: 48; text: "Refresh applications"; onClicked: { shell.refreshApps(); menu.visible = false } palette.buttonText: shell.textColor; background: Rectangle { color: parent.hovered ? Qt.lighter(shell.panelColor, 1.5) : "transparent"; radius: 6 } }
            Button { width: parent.width; height: 48; text: "Show desktop"; onClicked: { shell.tasks.showDesktop(); menu.visible = false } palette.buttonText: shell.textColor; background: Rectangle { color: parent.hovered ? Qt.lighter(shell.panelColor, 1.5) : "transparent"; radius: 6 } }
        }
    }
}
