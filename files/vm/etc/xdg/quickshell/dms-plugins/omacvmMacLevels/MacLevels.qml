import QtQuick
import Quickshell
import Quickshell.Io
import qs.Common
import qs.Modules.OSD
import qs.Modules.Plugins

// The Bridge's "osd" events ({"type": "osd", "kind", "value", "muted", "source"}) come from `omacvm-bridge events`, which reads the
// session's omacvm-bridge-events socket. The VM's own volume and brightness stay with Dux Shell's built-in popups.
PluginComponent {
    id: root

    property string kind: "volume"
    property int level: 0
    property bool muted: false

    function iconFor(kind, level, muted) {
        if (kind === "brightness")
            return level < 34 ? "brightness_low" : level < 67 ? "brightness_medium" : "brightness_high";
        if (kind === "keyboard")
            return "keyboard";
        if (muted || level === 0)
            return "volume_off";
        return level < 50 ? "volume_down" : "volume_up";
    }

    function showEvent(data) {
        let e;
        try {
            e = JSON.parse(data);
        } catch (err) {
            return;
        }
        if (e.type !== "osd" || (e.source !== "keys" && e.source !== "api"))
            return;
        // Volume has Dux Shell's own popup: the "Mac" output follows the Mac (omacvm-mac-volume).
        const kind = e.kind;
        if (!["brightness", "keyboard"].includes(kind))
            return;
        root.kind = kind;
        root.level = Math.round(e.value ?? 0);
        root.muted = e.muted === true;
        for (const osd of popups.instances)
            osd.show();
    }

    Variants {
        id: popups
        model: SettingsData.getFilteredScreens("osd")

        delegate: LevelOSD {
            osdKind: root.kind === "volume" ? "volume" : "brightness"
            iconName: root.iconFor(root.kind, root.level, root.muted)
            endIconName: "laptop_mac"
            endIconLabel: root.kind === "volume" ? "Mac volume" : root.kind === "keyboard" ? "Mac keyboard light" : "Mac brightness"
            value: root.level
            displayText: root.kind === "volume" && root.muted ? I18n.tr("Muted") : ""

            onLevelRequested: level => {
                if (root.kind === "keyboard")
                    return;
                root.level = level;
                setLevel.command = ["omacvm-bridge", root.kind === "volume" ? "volume" : "brightness", String(level)];
                setLevel.running = true;
            }
        }
    }

    Process {
        id: setLevel
    }

    Process {
        id: events
        command: ["omacvm-bridge", "events"]
        running: true

        stdout: SplitParser {
            onRead: line => {
                if (line.startsWith("data: "))
                    root.showEvent(line.slice(6));
            }
        }

        // The stream ends while the Mac sleeps or the Bridge restarts.
        onExited: restart.start()
    }

    Timer {
        id: restart
        interval: 3000
        onTriggered: events.running = true
    }
}
