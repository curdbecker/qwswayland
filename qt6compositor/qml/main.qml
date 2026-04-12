// main.qml - Qt6 Wayland compositor root scene
// SPDX-License-Identifier: MIT

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtWayland.Compositor
import QtWayland.Compositor.QtShell
import QtWayland.Compositor.XdgShell

WaylandCompositor {
    id: waylandCompositor

    WaylandOutput {
        id: output

        compositor: waylandCompositor

        property ListModel shellSurfaces: ListModel {}
        function handleShellSurface(shellSurface) {
            shellSurfaces.append({ "shellSurface": shellSurface })
        }

        property var isLinuxFB: Qt.platform.pluginName.startsWith("linuxfb")

        // Note: Without sizeFollowsWindow set to true, Qt's compositor does not send
        //       the wl_output_mode event to a client. Therefore, better keep this true if
        //       you want to use qwswayland.
        sizeFollowsWindow: true

        window: Window {
            visible: true
            color: "#00000000"

            width:  output.isLinuxFB ? Screen.width  : 1920
            height: output.isLinuxFB ? Screen.height : 1080
            visibility: output.isLinuxFB ? Window.FullScreen : Window.Windowed

            Component.onCompleted: console.log("Window ready:", width, "x", height,
                                               "visibility:", visibility,
                                               "isLinuxFB:", output.isLinuxFB,
                                               "Screen:", Screen.width, "x", Screen.height)

            Repeater {
                id: chromeRepeater
                model: output.shellSurfaces

                QtShellChrome {
                    id: chrome

                    onClientDestroyed: {
                        output.shellSurfaces.remove(index)
                    }

                    ShellSurfaceItem {
                        id: shellSurfaceItemId

                        shellSurface: modelData
                        moveItem: chrome

                        // staysOnBottom: modelData.windowFlags & Qt.WindowStaysOnBottomHint
                        // staysOnTop: !staysOnBottom
                        //             && (modelData.windowFlags & Qt.WindowStaysOnTopHint)
                    }
                    shellSurfaceItem: shellSurfaceItemId
                }
            }

            // The alt-tab overlay (on top of everything)
            AltTabSwitcher {
                id: altTabSwitcher
                compositor: waylandCompositor
                shellSurfaces: output.shellSurfaces
                chromeRepeater: chromeRepeater
            }

            // Global shortcut to trigger it
            Shortcut {
                sequence: "Alt+Tab"
                context: Qt.ApplicationShortcut
                onActivated: {
                    if (!altTabSwitcher.visible)
                        altTabSwitcher.show()
                    else
                        altTabSwitcher.cycleForward()
                }
            }
        }
    }

    QtShell {
        onQtShellSurfaceCreated: qtShellSurface => output.handleShellSurface(
                                     qtShellSurface)
    }
    XdgShell {
        onToplevelCreated: (toplevel, xdgSurface) => output.handleShellSurface(
                               xdgSurface)
    }

}
