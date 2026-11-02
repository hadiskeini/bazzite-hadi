import QtQuick
import Quickshell.Io
import qs.Common
import qs.Modules.ControlCenter
import qs.Modules.ControlCenter.Widgets
import qs.Modules.Plugins

// Control Center tile for the Mac: the tile mutes, its panel has the Mac's volume and brightness as the Control
// Center's own slider rows. Values come from the Bridge's "audio" ({"output": {"volume", "muted"}}) and "display"
// ({"brightness"}) events, 0 to 1, and from its "osd" events, 0 to 100.
PluginComponent {
    id: root

    property int volume: 0
    property bool muted: false
    property int brightness: 0
    property bool connected: false

    function volumeIcon(level, muted) {
        if (muted || level === 0)
            return "volume_off";
        return level < 50 ? "volume_down" : "volume_up";
    }

    function brightnessIcon(level) {
        return level < 34 ? "brightness_low" : level < 67 ? "brightness_medium" : "brightness_high";
    }

    function bridge(args) {
        const p = runner.createObject(root, {
            command: ["omacvm-bridge"].concat(args)
        });
        p.running = true;
    }

    function update(data) {
        let e;
        try {
            e = JSON.parse(data);
        } catch (err) {
            return;
        }
        root.connected = true;
        if (e.type === "osd") {
            if (e.kind === "volume" || e.kind === "mute") {
                root.volume = Math.round(e.value ?? root.volume);
                root.muted = e.muted === true;
            } else if (e.kind === "brightness") {
                root.brightness = Math.round(e.value ?? root.brightness);
            }
        } else if (e.output && typeof e.output.volume === "number") {
            root.volume = Math.round(e.output.volume * 100);
            root.muted = e.output.muted === true;
        } else if (typeof e.brightness === "number") {
            root.brightness = Math.round(e.brightness * 100);
        }
    }

    ccWidgetIcon: volumeIcon(volume, muted)
    ccWidgetPrimaryText: "Mac"
    ccWidgetSecondaryText: !connected ? I18n.tr("Not connected") : muted ? `${I18n.tr("Muted")} · ${brightness}%` : `${volume}% · ${brightness}%`
    ccWidgetIsActive: connected && !muted
    onCcWidgetToggled: bridge(["mute", "toggle"])

    ccDetailHeight: CcMetrics.gridRowUnit * 2 + Theme.spacingS
    ccDetailContent: Component {
        Column {
            spacing: Theme.spacingS

            CcSliderRow {
                id: volumeRow
                width: parent.width
                height: CcMetrics.gridRowUnit
                iconName: root.volumeIcon(root.volume, root.muted)
                iconLabel: root.muted ? I18n.tr("Unmute") : I18n.tr("Mute")
                sliderLabel: "Mac volume"
                minimum: 0
                maximum: 100
                sliderEnabled: root.connected
                onIconClicked: root.bridge(["mute", "toggle"])
                onSliderValueChanged: newValue => root.bridge(["volume", String(newValue)])

                Binding {
                    target: volumeRow.slider
                    property: "value"
                    value: root.volume
                    restoreMode: Binding.RestoreNone
                    when: !volumeRow.isDragging
                }
            }

            CcSliderRow {
                id: brightnessRow
                width: parent.width
                height: CcMetrics.gridRowUnit
                iconName: root.brightnessIcon(root.brightness)
                sliderLabel: "Mac brightness"
                minimum: 0
                maximum: 100
                sliderEnabled: root.connected
                onSliderValueChanged: newValue => root.bridge(["brightness", String(newValue)])

                Binding {
                    target: brightnessRow.slider
                    property: "value"
                    value: root.brightness
                    restoreMode: Binding.RestoreNone
                    when: !brightnessRow.isDragging
                }
            }
        }
    }

    Component {
        id: runner
        Process {
            onExited: destroy()
        }
    }

    Process {
        id: events
        command: ["omacvm-bridge", "events"]
        running: true

        stdout: SplitParser {
            onRead: line => {
                if (line.startsWith("data: "))
                    root.update(line.slice(6));
            }
        }

        onExited: {
            root.connected = false;
            restart.start();
        }
    }

    Timer {
        id: restart
        interval: 3000
        onTriggered: events.running = true
    }
}
