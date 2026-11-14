// The optional look of the phone: setting de.yesman.GnomePhone.Theme name ('default' | 'renaissance'). Read once when
// the shell loads the extension (palette.js, text.js): ES modules keep what they computed at load, so the shell
// follows a change at its next start; the Neo apps follow it live (libneo).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

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

// The theme's wallpaper (a Neo parallax set, if installed) takes the place of the picture in use while the look is on;
// the old one is kept in the setting previous-wallpaper and put back when the look goes. Follows the setting live.
export class ThemeWallpaper {
    constructor() {
        this._bg = null;
        if (!settings)
            return;
        try {
            this._bg = new Gio.Settings({schema_id: 'org.gnome.desktop.background'});
        } catch (e) {
            return;
        }
        settings.connectObject('changed::name', () => this._apply(isRenaissance()), this);
    }

    destroy() {
        settings?.disconnectObject(this);
    }

    _mine() {
        const dirs = [GLib.build_filenamev([GLib.get_user_data_dir(), 'neo-launcher', 'parallax']),
            '/usr/share/neo-launcher/parallax'];
        for (const dir of dirs) {
            const path = GLib.build_filenamev([dir, 'renaissance-cyber', 'wallpaper.jpg']);
            if (GLib.file_test(path, GLib.FileTest.EXISTS))
                return GLib.filename_to_uri(path, null);
        }
        return null;
    }

    _apply(on) {
        const mine = this._mine();
        if (!this._bg || !mine)
            return;
        const now = this._bg.get_string('picture-uri');
        if (on && now !== mine) {
            settings.set_string('previous-wallpaper', now);
            this._bg.set_string('picture-uri', mine);
            this._bg.set_string('picture-uri-dark', mine);
        } else if (!on && now === mine) {
            const before = settings.get_string('previous-wallpaper');
            if (before) {
                this._bg.set_string('picture-uri', before);
                this._bg.set_string('picture-uri-dark', before);
            }
            settings.set_string('previous-wallpaper', '');
        }
    }
}
