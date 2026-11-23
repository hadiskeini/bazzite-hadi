// Loaded into KWin by /usr/libexec/holdsuper-kde: reports the focused window to it over D-Bus, and binds the two
// keys that holdsuper sends during a hold (Meta+F12, Meta+F11) to minimize and close the window under the pointer.

const SERVICE = "local.hadi.HoldSuper";

function report() {
    const window = workspace.activeWindow;
    if (window) {
        callDBus(SERVICE, "/", SERVICE, "Focus", window.pid, window.resourceClass, window.fullScreen);
    } else {
        callDBus(SERVICE, "/", SERVICE, "Focus", 0, "", false);
    }
}

let watched = null;

function onActivated(window) {
    if (watched) {
        try {
            watched.fullScreenChanged.disconnect(report);
        } catch (error) {
        }
    }
    watched = window;
    if (window) {
        window.fullScreenChanged.connect(report);
    }
    report();
}

workspace.windowActivated.connect(onActivated);
onActivated(workspace.activeWindow);

function windowUnderPointer() {
    const pointer = workspace.cursorPos;
    const order = workspace.stackingOrder;
    for (let i = order.length - 1; i >= 0; i--) {
        const window = order[i];
        if (!(window.normalWindow || window.dialog) || window.minimized) {
            continue;
        }
        if (!window.onAllDesktops && !window.desktops.includes(workspace.currentDesktop)) {
            continue;
        }
        const frame = window.frameGeometry;
        if (pointer.x >= frame.x && pointer.x < frame.x + frame.width &&
            pointer.y >= frame.y && pointer.y < frame.y + frame.height) {
            return window;
        }
    }
    return null;
}

registerShortcut("holdsuper-minimize", "holdsuper: Minimize the window under the pointer", "Meta+F12", function () {
    const window = windowUnderPointer();
    if (window && window.minimizable) {
        window.minimized = true;
    }
});

registerShortcut("holdsuper-close", "holdsuper: Close the window under the pointer", "Meta+F11", function () {
    const window = windowUnderPointer();
    if (window && window.closeable) {
        window.closeWindow();
    }
});
