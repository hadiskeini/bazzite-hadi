pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Wayland
import qs.Common
import qs.Modules.Plugins

// An auto-hidden dock shows when the pointer is on the display's last row. OmacVM.app maps the Mac's
// last point to (n-1)/n of QEMU's tablet range, so the VM's pointer stops one row short (1115.98 on a
// display 1117 high). A strip two rows high, as wide as the dock, keeps the dock shown with the dock's
// own reveal while the pointer is on it.
PluginComponent {
    id: root

    Variants {
        model: Quickshell.screens

        delegate: Loader {
            id: edge

            required property var modelData

            // Dock windows are the tracked surfaces that carry an [output, config id] identity.
            readonly property var dock: SurfaceRecovery._windows.find(w => w.identity?.[0] === edge.modelData.name && w.visible && w.body?.config?.position === SettingsData.Position.Bottom && w.body.autoHide) ?? null
            readonly property var trigger: dock?.body.inputMaskItem ?? null

            active: trigger !== null && trigger.width > 0

            sourceComponent: PanelWindow {
                screen: edge.modelData
                color: "transparent"

                WlrLayershell.namespace: "omacvm:dock-edge"
                WlrLayershell.layer: WlrLayer.Top
                WlrLayershell.exclusionMode: ExclusionMode.Ignore
                WlrLayershell.keyboardFocus: WlrKeyboardFocus.None

                anchors {
                    bottom: true
                    left: true
                }
                // An auto-hidden dock ignores exclusive zones, so its window starts at the screen's left edge.
                margins.left: edge.trigger.x
                implicitWidth: edge.trigger.width
                implicitHeight: 2

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                // Shorter than the dock's own 250 ms hold, so the dock stays while the pointer rests here.
                Timer {
                    interval: 150
                    repeat: true
                    triggeredOnStart: true
                    running: hover.containsMouse
                    onTriggered: edge.dock?.body.revealWidgetItem(null)
                }
            }
        }
    }
}
