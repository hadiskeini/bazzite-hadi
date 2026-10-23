// Hold Super — GNOME Shell 50/51 (ESM).
//
// Front end for the holdsuper daemon (~/bazzite-system/portable/holdsuper): the daemon does the
// tap/hold on the mouse itself, this extension only tells it two things over
// $XDG_RUNTIME_DIR/holdsuper.sock:
//
//   - the settings (hold threshold, whether a lone hold opens Activities), and
//   - "pass 1" whenever a game is focused, "pass 0" otherwise. In pass mode the daemon closes the
//     mouse device, so a game sees the stock kernel -> libinput -> Mutter path with nothing added.
//
// Why not do it all in here: Mutter's own event filter is registered first (meta_display_init_events)
// and forwards the button to the Wayland client before any extension's Clutter filter runs, so an
// extension cannot hold a click back. Checked against mutter 51.0 clutter-event.c / events.c.
//
// When the shell disables us (logout, lock screen) the socket closes and the daemon falls back to
// pass mode by itself, so the mouse is never left grabbed without this side watching for games.
//
// Game detection, on every focus change (no polling):
//   - window class steam_app_<n> or *.exe (Proton/Wine, X11 or winewayland),
//   - the window's process or one of its ancestors runs a *.exe (Wine under Faugus/Flatpak, where
//     environ is unreadable but cmdline is) or is Steam's `reaper SteamLaunch` (native Steam games),
//   - the extra classes from the preferences, and optionally any fullscreen window.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const SOCKET_NAME = 'holdsuper.sock';
const RETRY_SECONDS = 2;
const MAX_ANCESTORS = 12;

const decoder = new TextDecoder();
const encoder = new TextEncoder();

function readProc(pid, file) {
    try {
        const [ok, bytes] = GLib.file_get_contents(`/proc/${pid}/${file}`);
        return ok ? decoder.decode(bytes) : null;
    } catch {
        return null;
    }
}

function parentPid(pid) {
    const stat = readProc(pid, 'stat');
    if (!stat)
        return 0;
    // "pid (comm) state ppid ..." — comm may contain spaces and parens, so split after the last ')'
    const fields = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
    return parseInt(fields[1]) || 0;
}

function processLooksLikeGame(pid) {
    for (let i = 0; i < MAX_ANCESTORS && pid > 1; i++) {
        const cmdline = readProc(pid, 'cmdline');
        if (cmdline) {
            const argv = cmdline.split('\0');
            if (/\.exe$/i.test(argv[0]))
                return true;
            if (argv.includes('SteamLaunch'))
                return true;
        }
        pid = parentPid(pid);
    }
    return false;
}

export default class HoldSuperExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._socketPath = GLib.build_filenamev([GLib.get_user_runtime_dir(), SOCKET_NAME]);
        this._conn = null;
        this._cancellable = null;
        this._retryId = 0;
        this._sentPass = null;
        this._focusWindow = null;
        this._windowSignals = [];

        this._settingsIds = [
            this._settings.connect('changed::hold-ms', () => this._sendSettings()),
            this._settings.connect('changed::overview-on-release', () => this._sendSettings()),
            this._settings.connect('changed::fullscreen-is-game', () => this._update()),
            this._settings.connect('changed::game-classes', () => this._update()),
        ];
        this._focusId = global.display.connect('notify::focus-window', () => this._onFocus());

        this._onFocus();
        this._connect();
    }

    disable() {
        global.display.disconnect(this._focusId);
        this._unwatchWindow();
        for (const id of this._settingsIds)
            this._settings.disconnect(id);
        this._settingsIds = null;
        if (this._retryId) {
            GLib.source_remove(this._retryId);
            this._retryId = 0;
        }
        this._closeConnection();
        this._settings = null;
    }

    // ── connection ────────────────────────────────────────────────────────────

    _connect() {
        this._cancellable = new Gio.Cancellable();
        const cancellable = this._cancellable;
        const client = new Gio.SocketClient();
        client.connect_async(Gio.UnixSocketAddress.new(this._socketPath), cancellable, (c, res) => {
            try {
                this._conn = c.connect_finish(res);
            } catch (e) {
                if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    this._scheduleRetry();
                return;
            }
            this._sentPass = null;
            this._sendSettings();
            this._update();
            this._watchForClose(cancellable);
        });
    }

    // The daemon never writes; a read that completes means the socket closed (daemon restarted).
    _watchForClose(cancellable) {
        this._conn.get_input_stream().read_bytes_async(64, GLib.PRIORITY_DEFAULT, cancellable, (stream, res) => {
            try {
                stream.read_bytes_finish(res);
            } catch (e) {
                if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    return;
            }
            this._closeConnection();
            this._scheduleRetry();
        });
    }

    _scheduleRetry() {
        if (this._retryId || !this._settings)
            return;
        this._retryId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, RETRY_SECONDS, () => {
            this._retryId = 0;
            this._connect();
            return GLib.SOURCE_REMOVE;
        });
    }

    _closeConnection() {
        this._cancellable?.cancel();
        this._cancellable = null;
        if (this._conn) {
            try {
                this._conn.close(null);
            } catch {}
            this._conn = null;
        }
    }

    _send(text) {
        if (!this._conn)
            return false;
        try {
            this._conn.get_output_stream().write_bytes(new GLib.Bytes(encoder.encode(text)), null);
            return true;
        } catch (e) {
            logError(e, 'hold-super: send failed');
            this._closeConnection();
            this._scheduleRetry();
            return false;
        }
    }

    _sendSettings() {
        const hold = this._settings.get_int('hold-ms');
        const overview = this._settings.get_boolean('overview-on-release') ? 1 : 0;
        this._send(`hold ${hold}\noverview ${overview}\n`);
    }

    // ── focus / game detection ────────────────────────────────────────────────

    _onFocus() {
        this._unwatchWindow();
        const win = global.display.focus_window;
        if (win) {
            this._focusWindow = win;
            // fullscreen toggles and late wm_class assignment don't change focus
            this._windowSignals = ['notify::fullscreen', 'notify::wm-class', 'unmanaged']
                .map(sig => win.connect(sig, () => this._update()));
        }
        this._update();
    }

    _unwatchWindow() {
        if (this._focusWindow) {
            for (const id of this._windowSignals) {
                try {
                    this._focusWindow.disconnect(id);
                } catch {}
            }
        }
        this._focusWindow = null;
        this._windowSignals = [];
    }

    _isGame(win) {
        const cls = (win.get_wm_class() ?? '').toLowerCase();
        if (/^steam_app_\d+$/.test(cls) || cls.endsWith('.exe'))
            return true;
        if (cls && this._settings.get_strv('game-classes').some(c => c.trim().toLowerCase() === cls))
            return true;
        if (this._settings.get_boolean('fullscreen-is-game') && win.is_fullscreen())
            return true;
        const pid = win.get_pid();
        return pid > 0 && processLooksLikeGame(pid);
    }

    _update() {
        const win = this._focusWindow;
        const pass = win && !win.unmanaging && this._isGame(win) ? 1 : 0;
        if (pass === this._sentPass)
            return;
        if (this._send(`pass ${pass}\n`))
            this._sentPass = pass;
    }
}
