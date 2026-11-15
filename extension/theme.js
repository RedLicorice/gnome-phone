// The optional look of the phone: setting de.yesman.GnomePhone.Theme name ('default' | 'renaissance'), and with it
// the four looks of the Renaissance theme (docs/theme.md): marble, sea (day), forest, plum (night). The effective look is
// the Theme setting's day-look while the system style (org.gnome.desktop.interface color-scheme) is light, its
// night-look while it is dark. Everything follows it live: currentLook() reads the settings on every call,
// onLookChanged() tells a surface when to restyle (palette.js's onRestyle is the one the surfaces use).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const SCHEMA = 'de.yesman.GnomePhone.Theme';
const DAY_LOOKS = ['marble', 'sea'];
const NIGHT_LOOKS = ['forest', 'plum'];

function openSettings(id) {
    try {
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup(id, true);
        return schema ? new Gio.Settings({settings_schema: schema}) : null;
    } catch (e) {
        logError(e, `gnome-phone: settings ${id}`);
        return null;
    }
}

const settings = openSettings(SCHEMA);
const interfaceSettings = openSettings('org.gnome.desktop.interface');

export function themeName() {
    return settings?.get_string('name') ?? 'default';
}

export function isRenaissance() {
    return themeName() === 'renaissance';
}

function isDark() {
    return interfaceSettings?.get_string('color-scheme') === 'prefer-dark';
}

// The effective look: 'marble' | 'sea' | 'forest' | 'plum'; null while the theme is off (the stock look)
export function currentLook() {
    if (!isRenaissance())
        return null;
    const dark = isDark();
    const look = settings.get_string(dark ? 'night-look' : 'day-look');
    return (dark ? NIGHT_LOOKS : DAY_LOOKS).includes(look) ? look : (dark ? 'forest' : 'marble');
}

// Whether the effective look is a day (light) look; false in the stock look, which is dark
export function isDay() {
    return DAY_LOOKS.includes(currentLook());
}

// obj -> callback(look). One set of connections for all of them, made with the first and dropped with the last one;
// the settings of a change (name, day-look and night-look written one after the other, as the theme picker does) are
// coalesced into one call from an idle, and a call is made only when the effective look is another one.
const listeners = new Map();
let lastLook = undefined;
let pending = 0;
let handlers = [];          // [settings, id] while connected

function flush() {
    pending = 0;
    const look = currentLook();
    if (look === lastLook)
        return GLib.SOURCE_REMOVE;
    lastLook = look;
    for (const [obj, callback] of [...listeners]) {
        if (!listeners.has(obj))
            continue;
        try {
            callback(look);
        } catch (e) {
            logError(e, 'gnome-phone: look change');
        }
    }
    return GLib.SOURCE_REMOVE;
}

function schedule() {
    if (!pending)
        pending = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, flush);
}

function connect() {
    if (handlers.length)
        return;
    lastLook = currentLook();
    for (const [object, signals] of [[settings, ['name', 'day-look', 'night-look']], [interfaceSettings, ['color-scheme']]]) {
        for (const key of signals) {
            if (object)
                handlers.push([object, object.connect(`changed::${key}`, schedule)]);
        }
    }
}

function disconnect() {
    for (const [object, id] of handlers)
        object.disconnect(id);
    handlers = [];
    if (pending)
        GLib.source_remove(pending);
    pending = 0;
}

// callback(look) when the effective look changes (look: the new currentLook(), null for the stock look); obj
// identifies the listener (any object, one callback each); undo with offLookChanged(obj)
export function onLookChanged(obj, callback) {
    listeners.set(obj, callback);
    connect();
}

export function offLookChanged(obj) {
    listeners.delete(obj);
    if (!listeners.size)
        disconnect();
}

// Font chain of a role ('display' | 'body' | 'mono') from the setting, as a Pango family list; '' when the role keeps
// the stock face. Callers append the stock family, so a missing font never blanks text. Read at shell start like the
// rest of the shell's theme.
export function fontChain(role) {
    try {
        return settings?.get_string(`font-${role}`).trim() ?? '';
    } catch (e) {
        return '';
    }
}

// The look's wallpaper (a Neo parallax set's still, for GNOME's lock screen and shade) and icon pack (both if
// installed) take the place of the ones in use while the theme is on, and follow every change of the look (day or
// night, marble or sea...). The wallpaper and the icon pack in use before are kept in previous-wallpaper /
// previous-icon-pack, only when what was in use is not one of ours (so swapping between looks never overwrites them),
// and are put back when the theme goes off. Follows the settings live.
const OURS_WALLPAPER = /\/neo-launcher\/parallax\/renaissance-[^/]+\/wallpaper\.jpg$/;
const OURS_PACK = /^renaissance(-|$)/;       // 'renaissance' (v1-v3) and the renaissance-day / -night / -glyph-* packs

export class ThemeSwaps {
    constructor() {
        this._bg = openSettings('org.gnome.desktop.background');
        this._neo = openSettings('de.yesman.neo');
        if (!settings)
            return;
        onLookChanged(this, () => this._apply());
        this._apply();
    }

    destroy() {
        offLookChanged(this);
    }

    _apply() {
        const look = currentLook();
        this._wallpaper(look);
        this._iconPack(look);
    }

    // the file of a look's still, or null where it is not installed
    _still(look) {
        const dirs = [GLib.build_filenamev([GLib.get_user_data_dir(), 'neo-launcher', 'parallax']),
            '/usr/share/neo-launcher/parallax'];
        for (const dir of dirs) {
            const path = GLib.build_filenamev([dir, `renaissance-${look}`, 'wallpaper.jpg']);
            if (GLib.file_test(path, GLib.FileTest.EXISTS))
                return GLib.filename_to_uri(path, null);
        }
        return null;
    }

    _setWallpaper(uri) {
        for (const key of ['picture-uri', 'picture-uri-dark']) {
            if (this._bg.get_string(key) !== uri)
                this._bg.set_string(key, uri);
        }
    }

    _wallpaper(look) {
        if (!this._bg)
            return;
        const now = this._bg.get_string('picture-uri');
        if (look) {
            const mine = this._still(look);
            if (!mine)
                return;
            if (!this._ours(now))
                settings.set_string('previous-wallpaper', now);
            this._setWallpaper(mine);
        } else if (this._ours(now)) {
            const before = settings.get_string('previous-wallpaper');
            if (before)
                this._setWallpaper(before);
            settings.set_string('previous-wallpaper', '');
        }
    }

    _ours(uri) {
        try {
            return OURS_WALLPAPER.test(GLib.filename_from_uri(uri)[0]);
        } catch (e) {
            return false;
        }
    }

    _iconPack(look) {
        if (!this._neo)
            return;
        const now = this._neo.get_string('icon-pack');
        if (look) {
            const pack = DAY_LOOKS.includes(look) ? 'renaissance-day' : 'renaissance-night';
            const dirs = [GLib.get_user_data_dir(), ...GLib.get_system_data_dirs()];
            if (!dirs.some(d => GLib.file_test(GLib.build_filenamev([d, 'neo-launcher', 'iconpacks', pack, 'pack.json']),
                GLib.FileTest.EXISTS)))
                return;
            if (!OURS_PACK.test(now))
                settings.set_string('previous-icon-pack', now);
            if (now !== pack)
                this._neo.set_string('icon-pack', pack);
        } else if (OURS_PACK.test(now)) {
            this._neo.set_string('icon-pack', settings.get_string('previous-icon-pack'));
            settings.set_string('previous-icon-pack', '');
        }
    }
}
