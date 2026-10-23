// EQ Preset Indicator — GNOME Shell 48–51 (ESM).
//
// Mirrors the WirePlumber smart-filter EQ presets switched by the system
// `eq-preset` script (~/dotfiles/bin/eq-preset). The script writes the active preset
// name to $XDG_RUNTIME_DIR/eq-preset.active; we watch that file so the tile
// tracks every change regardless of source (this menu, the
// <Super><Alt>m/h/e/b hotkeys, or a manual CLI invocation).
//
// Lives in the Quick Settings grid (like Caffeine / Power Mode) rather than as
// a permanent panel icon: a QuickMenuToggle whose subtitle is the active preset
// and whose menu jumps straight to any other one. Clicking the tile itself
// flips between the home preset (`default-preset`, else DEFAULT_PRESET) and the
// `click-preset` setting (DT-990 unless changed in the preferences); the tile is
// lit while that preset is active.
//
// A top-bar icon appears only while a preset other than the home preset is
// active, so the bar stays clean in the normal case but flags a diverging EQ at
// a glance.

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {PRESETS, homePreset} from './presets.js';

const EQ_PRESET = GLib.build_filenamev([GLib.get_home_dir(), '.local', 'bin', 'eq-preset']);

const FALLBACK_ICON = 'audio-card-symbolic';

// Module state outlives disable()/enable() (screen lock, Extensions app), so
// default-preset is applied once per shell session, not at every unlock.
let defaultPresetApplied = false;

const EqToggle = GObject.registerClass({
    Signals: {
        // Carries the active preset id, or '' when it is unknown.
        'preset-changed': {param_types: [GObject.TYPE_STRING]},
    },
},
class EqToggle extends QuickSettings.QuickMenuToggle {
    _init(extensionPath, settings) {
        super._init({
            title: 'EQ Preset',
            // `checked` is driven from the state file (lit = click preset
            // active), never flipped locally by the click.
            toggleMode: false,
        });

        this._extensionPath = extensionPath;
        this._settings = settings;
        this._active = null;

        for (const p of PRESETS)
            p._gicon = this._giconFor(p.icon);

        this.menu.setHeader(new Gio.ThemedIcon({name: FALLBACK_ICON}), 'EQ Preset');
        this._buildMenu();
        this._watchState();
        this._settingsIds = ['click-preset', 'default-preset'].map(
            key => this._settings.connect(`changed::${key}`, () => this._refresh()));
        this.connect('clicked', () => this._onClicked());
        this._refresh();
    }

    _clickPreset() {
        const id = this._settings.get_string('click-preset');
        return PRESETS.some(p => p.id === id) ? id : null;
    }

    _onClicked() {
        const target = this._clickPreset();
        if (!target) {
            this.menu.open();
            return;
        }
        this._select(this._active === target ? homePreset(this._settings) : target);
    }

    _giconFor(icon) {
        if (typeof icon === 'string')
            return new Gio.ThemedIcon({name: icon});
        // Bundled SVG. St recolours a FileIcon whose basename ends in
        // "-symbolic" to the foreground; non-symbolic names keep their
        // authored colours (used for the two-tone wired-earbuds icon).
        const path = GLib.build_filenamev([this._extensionPath, 'icons', `${icon.file}.svg`]);
        return new Gio.FileIcon({file: Gio.File.new_for_path(path)});
    }

    _buildMenu() {
        this._items = new Map();
        for (const p of PRESETS) {
            const item = new PopupMenu.PopupImageMenuItem(p.label, p._gicon);
            item.connect('activate', () => this._select(p.id));
            this.menu.addMenuItem(item);
            this._items.set(p.id, item);
        }
    }

    _statePath() {
        const dir = GLib.getenv('XDG_RUNTIME_DIR') || '/tmp';
        return GLib.build_filenamev([dir, 'eq-preset.active']);
    }

    _watchState() {
        this._stateFile = Gio.File.new_for_path(this._statePath());
        this._monitor = this._stateFile.monitor_file(Gio.FileMonitorFlags.NONE, null);
        this._monitorId = this._monitor.connect('changed', () => this._refresh());
    }

    _readActive() {
        try {
            const [ok, bytes] = this._stateFile.load_contents(null);
            if (!ok)
                return null;
            const txt = new TextDecoder().decode(bytes).trim();
            return PRESETS.some(p => p.id === txt) ? txt : null;
        } catch {
            return null; // file not created yet
        }
    }

    get activePreset() {
        return this._active;
    }

    _refresh() {
        const active = this._readActive();
        const preset = PRESETS.find(p => p.id === active);
        this._active = active;
        this._applyDefaultPreset(active);

        this.gicon = preset ? preset._gicon : new Gio.ThemedIcon({name: FALLBACK_ICON});
        this.subtitle = preset ? preset.label : 'None';
        this.menu.setHeader(this.gicon, 'EQ Preset', this.subtitle);
        // The home preset as the click target would make the tile a one-way
        // button, so it never lights up.
        const target = this._clickPreset();
        this.checked = !!target && target !== homePreset(this._settings) && active === target;

        for (const [id, item] of this._items)
            item.setOrnament(id === active ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);

        this.emit('preset-changed', active ?? '');
    }

    // Waits for the first state from eq-default.service, so the filters exist
    // and the login unit does not overwrite the preset afterwards.
    _applyDefaultPreset(active) {
        if (defaultPresetApplied || !active)
            return;
        defaultPresetApplied = true;
        const id = this._settings.get_string('default-preset');
        if (id !== active && PRESETS.some(p => p.id === id))
            this._select(id);
    }

    _select(id) {
        // Don't optimistically update — let the state-file monitor drive the UI
        // so the tile only ever reflects what eq-preset actually applied.
        try {
            Gio.Subprocess.new([EQ_PRESET, id], Gio.SubprocessFlags.NONE);
        } catch (e) {
            Main.notifyError('EQ Preset Indicator', `Failed to run eq-preset: ${e.message}`);
        }
    }

    destroy() {
        this._settingsIds?.forEach(id => this._settings.disconnect(id));
        this._settingsIds = null;
        if (this._monitor) {
            this._monitor.disconnect(this._monitorId);
            this._monitor.cancel();
            this._monitor = null;
        }
        super.destroy();
    }
});

// The tile always sits in the Quick Settings grid; the top-bar icon is added
// but kept hidden, and only shown while a non-default preset is active.
const EqIndicator = GObject.registerClass(
class EqIndicator extends QuickSettings.SystemIndicator {
    _init(extensionPath, settings) {
        super._init();

        this._icon = this._addIndicator();
        this._icon.visible = false;
        this._settings = settings;

        const toggle = new EqToggle(extensionPath, settings);
        toggle.connect('preset-changed', (_t, id) => this._syncIcon(id));
        this.quickSettingsItems.push(toggle);

        // The toggle refreshes once during construction, before the handler
        // above existed — catch up on whatever it already read.
        this._syncIcon(toggle.activePreset ?? '');
    }

    _syncIcon(id) {
        const preset = PRESETS.find(p => p.id === id);
        const diverged = preset && preset.id !== homePreset(this._settings);

        if (diverged)
            this._icon.gicon = preset._gicon;
        this._icon.visible = !!diverged;
    }
});

export default class EqIndicatorExtension extends Extension {
    enable() {
        this._settings = this.getSettings();
        this._indicator = new EqIndicator(this.path, this._settings);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator?.quickSettingsItems.forEach(item => item.destroy());
        this._indicator?.destroy();
        this._indicator = null;
        this._settings = null;
    }
}
