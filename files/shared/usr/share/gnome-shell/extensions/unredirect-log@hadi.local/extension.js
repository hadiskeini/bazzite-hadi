// Unredirect Log — GNOME Shell 50/51 (ESM).
//
// Mutter allows VRR and direct scanout only while the compositor-wide unredirect
// inhibit counter is 0. A JS caller that runs disable_unredirect() without the
// matching enable_unredirect() keeps every fullscreen window composited at a fixed
// refresh rate until the next login, with no visible sign.
//
// This wraps both methods and keeps the count since enable() in
// $XDG_RUNTIME_DIR/unredirect-log.json: {count, heldSince, outstanding[]}.
// `sys` (invariant unredirect-no-leak) fails when the count stays above 0 for
// longer than any animation or popup takes. After LEAK_SECONDS a notification names
// the holders and the outstanding callers go to the journal:  journalctl --user -o cat | grep unredirect-log
//
// session-modes includes unlock-dialog: a disable/enable cycle on lock would
// reset the count and hide a leak.
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const STATE_FILE = GLib.build_filenamev([GLib.get_user_runtime_dir(), 'unredirect-log.json']);
const LEAK_SECONDS = 30;

export default class UnredirectLog extends Extension {
    enable() {
        this._count = 0;
        this._heldSince = null;
        this._outstanding = [];
        this._writeId = 0;
        this._leakId = 0;
        this._im = new InjectionManager();
        const self = this;
        this._im.overrideMethod(Meta.Compositor.prototype, 'disable_unredirect', orig => function (...args) {
            self._onDisable();
            return orig.call(this, ...args);
        });
        this._im.overrideMethod(Meta.Compositor.prototype, 'enable_unredirect', orig => function (...args) {
            self._onEnable();
            return orig.call(this, ...args);
        });
        this._write();
    }

    disable() {
        this._im?.clear();
        this._im = null;
        for (const id of [this._writeId, this._leakId]) {
            if (id)
                GLib.source_remove(id);
        }
        this._writeId = this._leakId = 0;
        GLib.unlink(STATE_FILE);
    }

    _onDisable() {
        const frames = callerFrames();
        this._outstanding.push({file: fileOf(frames[0]), stack: frames.join(' <- '), at: now()});
        if (++this._count === 1) {
            this._heldSince = now();
            this._leakId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, LEAK_SECONDS, () => {
                this._leakId = 0;
                for (const o of this._outstanding)
                    console.log(`[unredirect-log] held ${now() - o.at}s, count=${this._count}: ${o.stack}`);
                // The banner itself takes and releases an inhibit; that pair is balanced and harmless.
                const who = this._outstanding.map(o => o.file).filter((f, i, a) => a.indexOf(f) === i).join(', ');
                Main.notify('VRR blocked: unredirect inhibit leaked',
                    `Held for ${LEAK_SECONDS}s by ${who || 'unknown'}. Log out and in, or drain it; see unredirect-log.json.`);
                return GLib.SOURCE_REMOVE;
            });
        }
        this._scheduleWrite();
    }

    _onEnable() {
        // An enable has no handle to its disable: drop the newest entry from the same
        // file (prepare/cleanup pairs live together), else the oldest.
        const file = fileOf(callerFrames()[0]);
        let i = this._outstanding.findLastIndex(o => o.file === file);
        if (i < 0 && this._outstanding.length)
            i = 0;
        if (i >= 0)
            this._outstanding.splice(i, 1);
        // Below 0 = the pair started before enable(); the real counter clamps at 0 too.
        this._count = Math.max(0, this._count - 1);
        if (this._count === 0) {
            this._heldSince = null;
            this._outstanding = [];
            if (this._leakId)
                GLib.source_remove(this._leakId);
            this._leakId = 0;
        }
        this._scheduleWrite();
    }

    _scheduleWrite() {
        this._writeId ||= GLib.idle_add(GLib.PRIORITY_LOW, () => {
            this._writeId = 0;
            this._write();
            return GLib.SOURCE_REMOVE;
        });
    }

    _write() {
        const state = {count: this._count, heldSince: this._heldSince, outstanding: this._outstanding};
        GLib.file_set_contents(STATE_FILE, JSON.stringify(state, null, 1));
    }
}

function now() {
    return Math.floor(GLib.get_real_time() / 1e6);
}

function callerFrames() {
    return new Error().stack.split('\n')
        .filter(l => l && !l.includes('unredirect-log@hadi.local'))
        .slice(0, 5)
        .map(l => l.replace(/^.*?@/, '').replace('resource:///org/gnome/shell/', 'shell:'));
}

function fileOf(frame) {
    return (frame ?? '').replace(/:\d+:\d+$/, '');
}
