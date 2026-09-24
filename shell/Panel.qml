import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property bool launcherOpen: false
    property int taskMenuId: -1
    property bool menuOpen: launcherOpen || taskMenuId >= 0
    onMenuOpenChanged: shellView.setExpanded(menuOpen)
    onLauncherOpenChanged: {
        if (launcherOpen) { taskMenuId = -1; search.text = ""; search.forceActiveFocus() }
    }
    function closeMenus() { launcherOpen = false; taskMenuId = -1 }
    Keys.onEscapePressed: closeMenus()

    MouseArea {
        anchors.fill: parent
        visible: root.menuOpen
        onClicked: root.closeMenus()
    }

    Rectangle {
        id: launcher
        visible: root.launcherOpen
        width: Math.min(460, root.width - 24)
        height: root.height - bar.height - 20
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.bottom: bar.top
        anchors.bottomMargin: 10
        color: shell.panelColor
        border.color: Qt.lighter(shell.panelColor, 1.65)
        radius: 14
        MouseArea { anchors.fill: parent }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Text { text: "Applications"; color: shell.textColor; font.pixelSize: 21; font.weight: Font.DemiBold }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Refresh"
                    onClicked: shell.refreshApps()
                    palette.buttonText: shell.textColor
                    background: Rectangle { color: parent.hovered ? "#304058" : "transparent"; radius: 6 }
                }
            }
            TextField {
                id: search
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                placeholderText: "Search applications"
                placeholderTextColor: Qt.darker(shell.textColor, 1.5)
                color: shell.textColor
                selectByMouse: true
                leftPadding: 12
                font.pixelSize: 14
                background: Rectangle {
                    radius: 7
                    color: Qt.darker(shell.panelColor, 1.2)
                    border.color: search.activeFocus ? shell.accent : Qt.lighter(shell.panelColor, 1.7)
                }
                onAccepted: {
                    if (applications.count > 0 && shell.launch(applications.model[0].appId)) root.closeMenus()
                }
                Keys.onEscapePressed: root.closeMenus()
            }
            ListView {
                id: applications
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3
                model: shell.apps.filter(function(app) {
                    return (app.name + " " + app.appId).toLowerCase().indexOf(search.text.toLowerCase()) >= 0
                })
                ScrollBar.vertical: ScrollBar {}
                delegate: Button {
                    required property var modelData
                    width: ListView.view.width - 10
                    height: 48
                    onClicked: { if (shell.launch(modelData.appId)) root.closeMenus() }
                    background: Rectangle { radius: 7; color: parent.hovered ? Qt.lighter(shell.panelColor, 1.55) : "transparent" }
                    contentItem: RowLayout {
                        spacing: 12
                        Image { source: "image://icons/" + modelData.icon; sourceSize: Qt.size(30, 30); Layout.preferredWidth: 30; Layout.preferredHeight: 30 }
                        Text { text: modelData.name; color: shell.textColor; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { visible: modelData.pinned; text: "Pinned"; color: shell.accent; font.pixelSize: 10 }
                    }
                }
                Text { anchors.centerIn: parent; visible: applications.count === 0; text: "No matching applications"; color: shell.textColor }
            }
            Text {
                Layout.fillWidth: true
                text: "shaoDe"
                color: Qt.darker(shell.textColor, 1.7)
                font.pixelSize: 11
            }
        }
    }

    Rectangle {
        visible: root.taskMenuId >= 0
        width: 220; height: 150
        anchors.left: parent.left; anchors.leftMargin: 100
        anchors.bottom: bar.top; anchors.bottomMargin: 8
        color: shell.panelColor; radius: 10
        border.color: Qt.lighter(shell.panelColor, 1.6)
        Column {
            anchors.fill: parent; anchors.margins: 6; spacing: 2
            Repeater {
                model: ["Maximize / restore", "Minimize", "Close window"]
                delegate: Button {
                    required property string modelData
                    required property int index
                    width: parent.width; height: 44
                    text: modelData
                    palette.buttonText: shell.textColor
                    background: Rectangle { color: parent.hovered ? Qt.lighter(shell.panelColor, 1.5) : "transparent"; radius: 6 }
                    onClicked: {
                        var id = root.taskMenuId
                        root.closeMenus()
                        if (index === 0) shell.tasks.maximize(id)
                        else if (index === 1) shell.tasks.minimize(id)
                        else shell.tasks.close(id)
                    }
                }
            }
        }
    }

    Rectangle {
        id: bar
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: shell.panelHeight
        color: shell.panelColor
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Qt.lighter(shell.panelColor, 1.65) }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
            spacing: 6
            Button {
                id: start
                Layout.preferredWidth: 44; Layout.preferredHeight: bar.height - 10
                onClicked: root.launcherOpen = !root.launcherOpen
                ToolTip.visible: hovered; ToolTip.text: "Applications"
                background: Rectangle { radius: 7; color: start.hovered || root.launcherOpen ? Qt.lighter(shell.panelColor, 1.8) : "transparent" }
                contentItem: Item {
                    Grid {
                        anchors.centerIn: parent; columns: 2; spacing: 3
                        Repeater { model: 4; Rectangle { width: 9; height: 9; radius: 2; color: shell.accent } }
                    }
                }
            }
            Repeater {
                model: shell.pinned
                delegate: Button {
                    required property var modelData
                    Layout.preferredWidth: 40; Layout.preferredHeight: bar.height - 10
                    onClicked: { if (shell.launch(modelData.appId)) root.closeMenus() }
                    ToolTip.visible: hovered; ToolTip.text: modelData.name
                    background: Rectangle { radius: 7; color: parent.hovered ? Qt.lighter(shell.panelColor, 1.55) : "transparent" }
                    contentItem: Image { source: "image://icons/" + modelData.icon; sourceSize: Qt.size(24, 24); fillMode: Image.PreserveAspectFit }
                }
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: Qt.lighter(shell.panelColor, 1.8) }
            ListView {
                id: taskList
                Layout.fillWidth: true; Layout.fillHeight: true
                orientation: ListView.Horizontal; spacing: 4; clip: true
                model: shell.tasks
                delegate: Button {
                    id: taskButton
                    required property int taskId
                    required property string title
                    required property string appId
                    required property bool active
                    required property bool minimized
                    width: Math.min(185, Math.max(92, taskList.width / Math.max(1, taskList.count) - 4))
                    height: bar.height - 10; y: 5
                    onClicked: { root.closeMenus(); shell.tasks.activate(taskId) }
                    ToolTip.visible: hovered; ToolTip.text: title
                    background: Rectangle {
                        radius: 6
                        color: taskButton.active ? Qt.lighter(shell.panelColor, 1.7) : (taskButton.hovered ? Qt.lighter(shell.panelColor, 1.4) : "transparent")
                        Rectangle { anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; width: taskButton.active ? 28 : 10; height: 3; radius: 1; color: taskButton.minimized ? "#627084" : shell.accent }
                    }
                    contentItem: RowLayout {
                        spacing: 6
                        Image { source: "image://icons/" + taskButton.appId; sourceSize: Qt.size(22, 22); Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
                        Text { text: taskButton.title; color: shell.textColor; elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: 12 }
                    }
                    TapHandler { acceptedButtons: Qt.RightButton; onTapped: { root.launcherOpen = false; root.taskMenuId = taskButton.taskId } }
                }
            }
            Text {
                id: clock
                property date now: new Date()
                text: Qt.formatTime(now, "HH:mm") + "\n" + Qt.formatDate(now, "ddd d MMM")
                color: shell.textColor; horizontalAlignment: Text.AlignRight; font.pixelSize: 11
                Layout.preferredWidth: 82
                Timer { interval: 1000; running: true; repeat: true; onTriggered: clock.now = new Date() }
            }
            Button {
                Layout.preferredWidth: 14; Layout.fillHeight: true
                onClicked: { root.closeMenus(); shell.tasks.showDesktop() }
                ToolTip.visible: hovered; ToolTip.text: "Show desktop"
                background: Rectangle { color: parent.hovered ? shell.accent : Qt.lighter(shell.panelColor, 1.6); width: 3; anchors.right: parent.right }
            }
        }
        Rectangle {
            visible: shell.error.length > 0
            anchors.fill: parent; anchors.margins: 4
            color: "#542b32"; radius: 6
            Text { anchors.left: parent.left; anchors.right: dismiss.left; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 10; text: shell.error; color: "#fff0f1"; elide: Text.ElideRight; font.pixelSize: 12 }
            Button { id: dismiss; anchors.right: parent.right; height: parent.height; width: 40; text: "×"; onClicked: shell.clearError() }
        }
    }
}
