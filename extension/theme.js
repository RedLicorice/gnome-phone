// The optional look of the phone: setting de.yesman.GnomePhone.Theme name ('default' | 'renaissance'). Read once when
// the shell loads the extension (palette.js, text.js): ES modules keep what they computed at load, so the shell
// follows a change at its next start; the Neo apps follow it live (libneo).
import Gio from 'gi://Gio';

const SCHEMA = 'de.yesman.GnomePhone.Theme';

let settings = null;
try {
    const source = Gio.SettingsSchemaSource.get_default();
    const schema = source?.lookup(SCHEMA, true);
    if (schema)
        settings = new Gio.Settings({settings_schema: schema});
} catch (e) {
    logError(e, 'gnome-phone: theme setting');
}

export function themeName() {
    return settings?.get_string('name') ?? 'default';
}

export function isRenaissance() {
    return themeName() === 'renaissance';
}
