// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property bool launcherOpen: false
    // The context menu belongs to a task, or to the bar itself when barMenuOpen is set.
    property int taskMenuId: -1
    property bool barMenuOpen: false
    property real contextMenuX: 0
    property bool menuOpen: launcherOpen || taskMenuId >= 0 || barMenuOpen
    onMenuOpenChanged: shellView.setExpanded(menuOpen)
    onLauncherOpenChanged: {
        if (launcherOpen) { taskMenuId = -1; barMenuOpen = false; search.text = ""; search.forceActiveFocus() }
    }
    function closeMenus() { launcherOpen = false; taskMenuId = -1; barMenuOpen = false }
    // Opens on press, as a desktop context menu does: waiting for a tap lost a press held
    // past the long-press time or moved while held. The new menu opens before the old one
    // closes, so the surface does not collapse in between.
    function openContextMenu(item, x, taskId) {
        contextMenuX = item.mapToItem(root, x, 0).x
        if (taskId >= 0) { taskMenuId = taskId; barMenuOpen = false }
        else { barMenuOpen = true; taskMenuId = -1 }
        launcherOpen = false
    }
    // Popups open away from the screen edge the bar sits on.
    // Tiling is per monitor: this panel shows and toggles its own.
    readonly property bool tiling: {
        var state = shell.workspaces[outputName]
        return state && state.tiling !== undefined ? state.tiling : shell.tiling
    }
    readonly property bool onTop: shell.panelTop
    readonly property string uiFont: shell.fontFamily.length > 0 ? shell.fontFamily : Qt.application.font.family
    readonly property bool floating: shell.panelRadius > 0 || shell.panelMarginLeft > 0 ||
                                     shell.panelMarginRight > 0 || shell.panelMarginTop > 0 ||
                                     shell.panelMarginBottom > 0
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
        height: root.height - shell.panelExtent - 20
        anchors.left: parent.left
        anchors.leftMargin: 12 + shell.panelMarginLeft
        anchors.bottom: root.onTop ? undefined : bar.top
        anchors.top: root.onTop ? bar.bottom : undefined
        anchors.bottomMargin: 10
        anchors.topMargin: 10
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
                Text { text: "Applications"; color: shell.textColor; font.pixelSize: 21; font.weight: Font.DemiBold; font.family: root.uiFont }
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
                objectName: "applicationSearch"
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                placeholderText: "Search applications"
                placeholderTextColor: Qt.darker(shell.textColor, 1.5)
                color: shell.textColor
                selectByMouse: true
                leftPadding: 12
                font.pixelSize: 14; font.family: root.uiFont
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
                        Text { text: modelData.name; color: shell.textColor; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true; font.family: root.uiFont }
                        Text { visible: modelData.pinned; text: "Pinned"; color: shell.accent; font.pixelSize: 10; font.family: root.uiFont }
                    }
                }
                Text { anchors.centerIn: parent; visible: applications.count === 0; text: "No matching applications"; color: shell.textColor; font.family: root.uiFont }
            }
            Text {
                Layout.fillWidth: true
                text: "shaoDe"
                color: Qt.darker(shell.textColor, 1.7)
                font.pixelSize: 11; font.family: root.uiFont
            }
        }
    }

    Rectangle {
        id: contextMenu
        objectName: "contextMenu"
        readonly property var actions: root.taskMenuId >= 0
            ? [{ text: "Maximize / restore", run: function(id) { shell.tasks.maximize(id) } },
               { text: "Minimize", run: function(id) { shell.tasks.minimize(id) } },
               { text: "Close window", run: function(id) { shell.tasks.close(id) } }]
            : [{ text: root.tiling ? "Turn tiling off" : "Turn tiling on", enabled: shell.tilingAvailable,
                 run: function() { shell.toggleTiling(outputName) } },
               { text: "Applications", run: function() { root.launcherOpen = true } },
               { text: "Show desktop", run: function() { shell.tasks.showDesktop() } }]
        visible: root.taskMenuId >= 0 || root.barMenuOpen
        width: 220; height: 12 + actions.length * 44 + (actions.length - 1) * 2
        x: Math.max(8, Math.min(root.contextMenuX, root.width - width - 8))
        anchors.bottom: root.onTop ? undefined : bar.top; anchors.bottomMargin: 8
        anchors.top: root.onTop ? bar.bottom : undefined; anchors.topMargin: 8
        color: shell.panelColor; radius: 10
        border.color: Qt.lighter(shell.panelColor, 1.6)
        MouseArea { anchors.fill: parent }
        Column {
            anchors.fill: parent; anchors.margins: 6; spacing: 2
            Repeater {
                model: contextMenu.actions
                delegate: Button {
                    required property var modelData
                    objectName: "contextMenuItem"
                    width: parent.width; height: 44
                    text: modelData.text
                    enabled: modelData.enabled !== false
                    opacity: enabled ? 1 : 0.4
                    palette.buttonText: shell.textColor
                    background: Rectangle { color: parent.hovered ? Qt.lighter(shell.panelColor, 1.5) : "transparent"; radius: 6 }
                    // Run before closing, so opening the launcher keeps the surface expanded.
                    onClicked: {
                        modelData.run(root.taskMenuId)
                        root.taskMenuId = -1; root.barMenuOpen = false
                    }
                }
            }
        }
    }

    Rectangle {
        id: bar
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: shell.panelMarginLeft; anchors.rightMargin: shell.panelMarginRight
        anchors.bottom: root.onTop ? undefined : parent.bottom
        anchors.top: root.onTop ? parent.top : undefined
        anchors.bottomMargin: shell.panelMarginBottom; anchors.topMargin: shell.panelMarginTop
        height: shell.panelHeight
        color: shell.panelColor
        radius: shell.panelRadius
        // A floating bar gets an outline; a docked one a line along its inner edge.
        border.width: root.floating ? 1 : 0
        border.color: Qt.lighter(shell.panelColor, 1.65)
        Rectangle {
            visible: !root.floating
            y: root.onTop ? parent.height - 1 : 0
            width: parent.width; height: 1; color: Qt.lighter(shell.panelColor, 1.65)
        }
        // Right-clicking the bar anywhere but on a task opens the bar's own menu.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onPressed: (mouse) => root.openContextMenu(bar, mouse.x, -1)
        }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
            spacing: 6
            Button {
                id: start
                Layout.preferredWidth: 44; Layout.preferredHeight: bar.height - 10
                onClicked: root.launcherOpen = !root.launcherOpen
                Accessible.name: "Applications"
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
                    Accessible.name: modelData.name
                    background: Rectangle { radius: 7; color: parent.hovered ? Qt.lighter(shell.panelColor, 1.55) : "transparent" }
                    contentItem: Image { source: "image://icons/" + modelData.icon; sourceSize: Qt.size(24, 24); fillMode: Image.PreserveAspectFit }
                }
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: Qt.lighter(shell.panelColor, 1.8) }
            ListView {
                id: taskList
                objectName: "taskList"
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
                    Accessible.name: title
                    background: Rectangle {
                        radius: 6
                        color: taskButton.active ? Qt.lighter(shell.panelColor, 1.7) : (taskButton.hovered ? Qt.lighter(shell.panelColor, 1.4) : "transparent")
                        Rectangle { anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; width: taskButton.active ? 28 : 10; height: 3; radius: 1; color: taskButton.minimized ? "#627084" : shell.accent }
                    }
                    contentItem: RowLayout {
                        spacing: 6
                        Image { source: "image://icons/" + taskButton.appId; sourceSize: Qt.size(22, 22); Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
                        Text { text: taskButton.title; color: shell.textColor; elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: shell.fontSize; font.family: root.uiFont }
                    }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        onPressed: root.openContextMenu(taskButton, 0, taskButton.taskId)
                    }
                }
            }
            // This output's workspaces: the current one highlighted, a dot under those with
            // windows. Scrolling pages through them; clicking a number switches to it.
            Row {
                id: workspaceIndicator
                objectName: "workspaceIndicator"
                readonly property var workspaceState: shell.workspaces[outputName] || ({ current: 1, occupied: [] })
                function show(number) {
                    if (number >= 1 && number <= shell.workspaceCount && number !== workspaceState.current)
                        shell.showWorkspace(outputName, number)
                }
                visible: shell.workspaceCount > 1
                spacing: 2
                Layout.alignment: Qt.AlignVCenter
                Repeater {
                    model: shell.workspaceCount
                    delegate: Button {
                        id: workspaceButton
                        required property int index
                        readonly property int number: index + 1
                        readonly property bool current: workspaceIndicator.workspaceState.current === number
                        readonly property bool occupied: workspaceIndicator.workspaceState.occupied.indexOf(number) >= 0
                        objectName: "workspace" + number
                        width: 26; height: bar.height - 14
                        onClicked: { root.closeMenus(); workspaceIndicator.show(number) }
                        Accessible.name: "Workspace " + number
                        background: Rectangle {
                            radius: 6
                            color: workspaceButton.current ? Qt.lighter(shell.panelColor, 1.8) : (workspaceButton.hovered ? Qt.lighter(shell.panelColor, 1.4) : "transparent")
                        }
                        contentItem: Item {
                            Text {
                                anchors.centerIn: parent
                                text: workspaceButton.number
                                color: workspaceButton.current ? shell.accent : shell.textColor
                                font.pixelSize: shell.fontSize; font.family: root.uiFont
                                font.weight: workspaceButton.current ? Font.DemiBold : Font.Normal
                            }
                            Rectangle {
                                visible: workspaceButton.occupied
                                anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom
                                width: 4; height: 4; radius: 2
                                color: workspaceButton.current ? shell.accent : shell.textColor
                            }
                        }
                    }
                }
                // A wheel notch (or a touchpad's worth of travel) moves one workspace, stopping
                // at either end; down or right goes to the next.
                WheelHandler {
                    property real travel: 0
                    onWheel: (event) => {
                        travel += event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                        var steps = travel > 0 ? Math.floor(travel / 120) : Math.ceil(travel / 120)
                        travel -= steps * 120
                        if (steps !== 0) {
                            var target = workspaceIndicator.workspaceState.current - steps
                            workspaceIndicator.show(Math.max(1, Math.min(shell.workspaceCount, target)))
                        }
                    }
                }
            }
            Button {
                id: tilingToggle
                objectName: "tilingToggle"
                Layout.preferredWidth: 40; Layout.preferredHeight: bar.height - 10
                enabled: shell.tilingAvailable
                opacity: enabled ? 1 : 0.4
                onClicked: { root.closeMenus(); shell.toggleTiling(outputName) }
                Accessible.name: root.tiling ? "Tiling on" : "Tiling off"
                background: Rectangle {
                    radius: 7
                    color: root.tiling ? Qt.lighter(shell.panelColor, 1.8) : (tilingToggle.hovered ? Qt.lighter(shell.panelColor, 1.55) : "transparent")
                }
                // On: a dwindle split in the accent colour. Off: two overlapping windows.
                contentItem: Item {
                    Item {
                        anchors.centerIn: parent; width: 22; height: 16
                        visible: root.tiling
                        Rectangle { width: 10; height: 16; radius: 2; color: shell.accent }
                        Rectangle { x: 12; width: 10; height: 7; radius: 2; color: shell.accent }
                        Rectangle { x: 12; y: 9; width: 10; height: 7; radius: 2; color: shell.accent }
                    }
                    Item {
                        anchors.centerIn: parent; width: 22; height: 16
                        visible: !root.tiling
                        Rectangle { width: 15; height: 11; radius: 2; color: "transparent"; border.color: shell.textColor; border.width: 2 }
                        Rectangle { x: 7; y: 5; width: 15; height: 11; radius: 2; color: tilingToggle.hovered ? Qt.lighter(shell.panelColor, 1.55) : shell.panelColor; border.color: shell.textColor; border.width: 2 }
                    }
                }
            }
            Text {
                id: clock
                property date now: new Date()
                text: Qt.formatTime(now, "HH:mm") + "\n" + Qt.formatDate(now, "ddd d MMM")
                color: shell.textColor; horizontalAlignment: Text.AlignRight
                font.pixelSize: Math.max(6, shell.fontSize - 1); font.family: root.uiFont
                Layout.preferredWidth: 82
                Timer { interval: 1000; running: true; repeat: true; onTriggered: clock.now = new Date() }
            }
            Button {
                Layout.preferredWidth: 14; Layout.fillHeight: true
                onClicked: { root.closeMenus(); shell.tasks.showDesktop() }
                Accessible.name: "Show desktop"
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
