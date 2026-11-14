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

// The theme's wallpaper (a Neo parallax set) and icon pack (both if installed) take the place of the ones in use while
// the look is on; the old ones are kept in previous-wallpaper / previous-icon-pack and put back when the look goes.
// Follows the setting live.
export class ThemeSwaps {
    constructor() {
        this._bg = null;
        this._neo = null;
        if (!settings)
            return;
        const open = id => {
            const schema = Gio.SettingsSchemaSource.get_default()?.lookup(id, true);
            return schema ? new Gio.Settings({settings_schema: schema}) : null;
        };
        this._bg = open('org.gnome.desktop.background');
        this._neo = open('de.yesman.neo');
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
        this._wallpaper(on);
        this._iconPack(on);
    }

    _iconPack(on) {
        const dirs = [GLib.get_user_data_dir(), ...GLib.get_system_data_dirs()];
        const have = dirs.some(d => GLib.file_test(GLib.build_filenamev([d, 'neo-launcher', 'iconpacks', 'renaissance', 'pack.json']),
            GLib.FileTest.EXISTS));
        if (!this._neo || !have)
            return;
        const now = this._neo.get_string('icon-pack');
        if (on && now !== 'renaissance') {
            settings.set_string('previous-icon-pack', now);
            this._neo.set_string('icon-pack', 'renaissance');
        } else if (!on && now === 'renaissance') {
            this._neo.set_string('icon-pack', settings.get_string('previous-icon-pack'));
            settings.set_string('previous-icon-pack', '');
        }
    }

    _wallpaper(on) {
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
