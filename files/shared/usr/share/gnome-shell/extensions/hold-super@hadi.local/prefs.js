// Hold Super — preferences (GNOME Shell 50/51, ESM).

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

export default class HoldSuperPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        const page = new Adw.PreferencesPage();

        const hold = new Adw.PreferencesGroup({
            title: 'Side button',
            description: 'A tap is the button\'s own click (browser Forward). Held past the threshold it is Super. ' +
                'Clicking another mouse button or scrolling while it is down makes it Super at once.',
        });
        const threshold = new Adw.SpinRow({
            title: 'Hold threshold',
            subtitle: 'Milliseconds before a press counts as Super',
            adjustment: new Gtk.Adjustment({lower: 50, upper: 2000, step_increment: 10, page_increment: 50}),
        });
        settings.bind('hold-ms', threshold, 'value', Gio.SettingsBindFlags.DEFAULT);
        hold.add(threshold);

        const overview = new Adw.SwitchRow({
            title: 'Open Activities on a lone hold',
            subtitle: 'Hold past the threshold and release without using it, like tapping Super on the keyboard',
        });
        settings.bind('overview-on-release', overview, 'active', Gio.SettingsBindFlags.DEFAULT);
        hold.add(overview);
        page.add(hold);

        const games = new Adw.PreferencesGroup({
            title: 'Games',
            description: 'While a game is focused the mouse is left completely alone (no added delay) and the button ' +
                'is just itself. Steam and Proton/Wine games are detected automatically.',
        });
        const fullscreen = new Adw.SwitchRow({
            title: 'Treat every fullscreen window as a game',
        });
        settings.bind('fullscreen-is-game', fullscreen, 'active', Gio.SettingsBindFlags.DEFAULT);
        games.add(fullscreen);

        const classes = new Adw.EntryRow({
            title: 'Extra game window classes (comma-separated)',
            show_apply_button: true,
            text: settings.get_strv('game-classes').join(', '),
        });
        classes.connect('apply', () => {
            const list = classes.text.split(',').map(s => s.trim()).filter(Boolean);
            settings.set_strv('game-classes', list);
        });
        games.add(classes);
        page.add(games);

        window.add(page);
    }
}
