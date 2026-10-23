// Shared by extension.js and prefs.js (prefs runs outside the shell, so it
// can't import extension.js). Keep the ids in sync with the click-preset and
// default-preset choices in the schema.

// Menu order. `icon` is either a themed icon name or {file: 'name'} for a
// bundled icon under icons/ (symbolic ones recolour to the foreground).
export const PRESETS = [
    {id: 'studio',     label: 'Studio Monitors (LP-6)',     icon: {file: 'eq-studio-monitor-symbolic'}},
    {id: 'boost',      label: 'Studio Monitors (Boost)',    icon: 'audio-volume-overamplified-symbolic'},
    {id: 'headphones', label: 'Headphones (DT-990)',        icon: 'audio-headphones-symbolic'},
    {id: 'earpods',    label: 'Headphones (EarPods)',       icon: {file: 'eq-earbuds'}},
    {id: 'off',        label: 'No EQ',                      icon: 'audio-speakers-symbolic'},
];

// The preset eq-default.service applies at login. While the home preset is
// active (or while the state is still unknown, which it is for the first
// seconds of a session until the login unit has seen the filter node) the top
// bar shows nothing. A click on the tile while the click preset is active also
// goes back to the home preset.
export const DEFAULT_PRESET = 'studio';

// The preset the tile and the top bar treat as normal: the default-preset of
// this host, else DEFAULT_PRESET.
export function homePreset(settings) {
    const id = settings.get_string('default-preset');
    return PRESETS.some(p => p.id === id) ? id : DEFAULT_PRESET;
}
