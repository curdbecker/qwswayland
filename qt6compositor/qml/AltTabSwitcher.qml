// AltTabSwitcher.qml - Alt-Tab window switcher overlay
// SPDX-License-Identifier: MIT
import QtQuick
import QtQuick.Controls
import WindowSwitcher 1.0

Item {
    id: switcher
    anchors.fill: parent
    visible: false
    z: 1000 // above everything

    // The shell surfaces model and chrome repeater must be passed in
    required property var shellSurfaces
    required property var chromeRepeater

    required property var compositor

    property int currentIndex: -1

    function show() {
        if (shellSurfaces.count === 0) return
        currentIndex = 0
        visible = true
        focusScope.forceActiveFocus()
    }

    function cycleForward() {
        if (shellSurfaces.count === 0)
            return
        currentIndex = (currentIndex + 1) % shellSurfaces.count
    }

    function commit() {
        if (currentIndex < 0 || currentIndex >= shellSurfaces.count) {
            return
        }
        var item = chromeRepeater.itemAt(currentIndex)
        if (!item) {
            return
        }

        if ((item.windowState & Qt.WindowMinimized) !== 0)
            item.toggleMinimized()

        var surface = shellSurfaces.get(currentIndex).surface

        item.activate()
        compositor.defaultSeat.keyboardFocus = surface
        dismiss()
    }

    function dismiss() {
        visible = false
        currentIndex = -1
    }

    // Semi-transparent backdrop
    Rectangle {
        anchors.fill: parent
        color: "#80000000"
    }

    // Centered switcher panel
    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.8, thumbnailRow.implicitWidth + 60)
        height: 160
        radius: 12
        color: "#e0202020"
        border.color: "#60ffffff"
        border.width: 1

        Row {
            id: thumbnailRow
            anchors.centerIn: parent
            spacing: 16

            Repeater {
                model: switcher.shellSurfaces

                Item {
                    id: item

                    width: 120
                    height: 120

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: index === switcher.currentIndex ? "#4000aaff" : "transparent"
                        border.color: index === switcher.currentIndex ? "#00aaff" : "#40ffffff"
                        border.width: index === switcher.currentIndex ? 2 : 1

                        Column {
                            anchors.centerIn: parent
                            spacing: 4

                            Rectangle {
                                width: 96
                                height: 64
                                color: "#30ffffff"
                                radius: 4
                                anchors.horizontalCenter: parent.horizontalCenter

                                SurfaceThumbnail {
                                    width: 96
                                    height: 64
                                    surface: modelData.surface
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text: (index + 1).toString()
                                    color: "white"
                                    font.pixelSize: 24
                                }
                            }

                            Text {
                                width: 108
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: modelData.toplevel ? modelData.toplevel.title : (modelData.windowTitle || "<NULL>")
                                color: "white"
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }
    }

    // Focus scope to capture key events while the switcher is open
    FocusScope {
        id: focusScope
        anchors.fill: parent
        focus: switcher.visible

        onActiveFocusChanged: {
            if (!activeFocus && switcher.visible)
                switcher.dismiss()
        }

        Keys.onPressed: function (event) {
            if (!switcher.visible)
                return

            // Alt+Tab while already open: cycle forward
            if (event.key === Qt.Key_Tab
                    && (event.modifiers & Qt.AltModifier)) {
                switcher.cycleForward()
                event.accepted = true
            } // Escape to cancel
            else if (event.key === Qt.Key_Escape) {
                switcher.dismiss()
                event.accepted = true
            }
        }

        Keys.onReleased: function (event) {
            if (!switcher.visible)
                return

            // When Alt is released, commit the selection
            if (event.key === Qt.Key_Alt) {
                switcher.commit()
                event.accepted = true
            }
        }
    }
}
