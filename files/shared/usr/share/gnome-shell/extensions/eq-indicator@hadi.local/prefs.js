// EQ Preset Indicator — preferences (GNOME Shell 48–51, ESM).

import Adw from 'gi://Adw';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

import {PRESETS, DEFAULT_PRESET, homePreset} from './presets.js';

// '' = no click action, the tile just opens the menu. The home preset is left
// out: as the click target it would make the tile a one-way button.
const clickChoices = settings => [
    ...PRESETS.filter(p => p.id !== homePreset(settings)),
    {id: '', label: 'Open the preset menu'},
];

// '' = keep the preset that eq-default.service applies.
const LOGIN_CHOICES = [{id: '', label: `No change (${PRESETS.find(p => p.id === DEFAULT_PRESET).label})`}, ...PRESETS];

export default class EqIndicatorPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage();

        const login = new Adw.PreferencesGroup({
            title: 'Login',
            description: 'The preset to switch to once at login. While it is active, the top bar shows no EQ icon.',
        });
        login.add(this._comboRow(settings, 'default-preset', 'Preset at login', () => LOGIN_CHOICES, 0));
        page.add(login);

        const group = new Adw.PreferencesGroup({title: 'Tile click'});
        const describe = () => {
            const home = PRESETS.find(p => p.id === homePreset(settings)).label;
            group.description = `Clicking the tile switches to the preset below; clicking it again while that ` +
                `preset is active goes back to ${home}. The arrow on the right always opens the full menu.`;
        };
        describe();
        settings.connect('changed::default-preset', describe);
        group.add(this._comboRow(settings, 'click-preset', 'Click switches to',
            () => clickChoices(settings), PRESETS.length - 1, 'default-preset'));
        page.add(group);

        window.add(page);
    }

    // `choices` returns the rows; it is called again when `rebuildOn` changes.
    // `fallback` is the row shown when the stored id is not in the rows.
    _comboRow(settings, key, title, choices, fallback, rebuildOn = null) {
        const row = new Adw.ComboRow({title});
        let rows = [];
        let rebuilding = false;
        const sync = () => {
            const i = rows.findIndex(c => c.id === settings.get_string(key));
            row.selected = i >= 0 ? i : fallback;
        };
        const rebuild = () => {
            rows = choices();
            // Replacing the model resets `selected`; that is not a user choice.
            rebuilding = true;
            row.model = Gtk.StringList.new(rows.map(c => c.label));
            sync();
            rebuilding = false;
        };
        rebuild();
        settings.connect(`changed::${key}`, sync);
        if (rebuildOn)
            settings.connect(`changed::${rebuildOn}`, rebuild);
        row.connect('notify::selected', () => {
            const id = rows[row.selected]?.id ?? '';
            if (!rebuilding && settings.get_string(key) !== id)
                settings.set_string(key, id);
        });
        return row;
    }
}
