// The touch keyboard's look, from its settings: a stylesheet written to the cache and loaded into the shell's theme
// (St reads it like the theme's own). Rewritten only when a look setting or the key size changes, since every load
// restyles the whole stage; the theme reloading (a theme switch) drops it, so it is loaded again then.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {isRenaissance} from '../theme.js';

// background, key, modifier key, pressed key, label, preview/popup background
const PALETTES = {
    dark: {bg: '#2e2e33', key: '#5d5d62', mod: '#45454a', pressed: '#78787e', text: '#ffffff', pop: '#4a4a50'},
    light: {bg: '#d1d3d9', key: '#ffffff', mod: '#b4b8c1', pressed: '#e6e7ea', text: '#151515', pop: '#f4f4f6'},
    black: {bg: '#000000', key: '#1c1c1e', mod: '#101011', pressed: '#3a3a3d', text: '#ffffff', pop: '#2a2a2d'},
    // the optional Renaissance look (theme.js): lapis-night keys with parchment labels, used for the default 'dark' style
    renaissance: {bg: '#08070d', key: '#1d2142', mod: '#13121e', pressed: '#33407f', text: '#f0e6d0', pop: '#262b57'},
    material: {bg: '#263238', key: '#37474f', mod: '#2c393f', pressed: '#4f636d', text: '#eceff1', pop: '#455a64'},
};

export class KeyboardStyle {
    // @settings the keyboard's Gio.Settings; @rowHeight () => the height of a row of keys in pixels
    constructor(settings, rowHeight) {
        this._settings = settings;
        this._rowHeight = rowHeight;
        this._dir = GLib.build_filenamev([GLib.get_user_cache_dir(), 'gnome-phone']);
        this._serial = 0;
        this._file = null;
        this._css = null;
        this._context = St.ThemeContext.get_for_stage(global.stage);
        // a load emits 'changed' itself (StTheme custom-stylesheets-changed): only a theme switch reloads it
        this._loading = false;
        this._context.connectObject('changed', () => {
            if (this._loading)
                return;
            this._file = null;
            this.update(true);
        }, this);
    }

    css() {
        const s = this._settings;
        const style = s.get_string('style');
        const p = (isRenaissance() && style === 'dark' ? PALETTES.renaissance : PALETTES[style]) ?? PALETTES.dark;
        const row = this._rowHeight();
        const font = Math.max(8, Math.round(row * 0.42 * s.get_int('label-scale') / 100));
        const radius = s.get_int('key-radius');
        const borders = s.get_boolean('key-borders');
        const key = borders ? p.key : 'transparent';
        const mod = borders ? p.mod : 'transparent';
        const shadow = borders ? 'box-shadow: 0 1px 0 0 rgba(0, 0, 0, 0.35);' : '';
        return `
.hk-view { background-color: ${p.bg}; }
.hk-key { color: ${p.text}; background-color: ${key}; border-radius: ${radius}px; font-size: ${font}px;
  font-weight: bold; ${shadow} }
.hk-key.hk-mod { background-color: ${mod}; font-size: ${Math.round(font * 0.72)}px; font-weight: normal; }
.hk-key.hk-space { font-size: ${Math.round(font * 0.6)}px; font-weight: normal; color: ${p.text}88; }
.hk-key:active { background-color: ${p.pressed}; }
.hk-key:checked { background-color: -st-accent-color; color: #ffffff; }
.hk-key.hk-locked { background-color: -st-accent-color; color: #ffffff; border: 2px solid #ffffff; }
.hk-key StIcon { icon-size: ${Math.round(font * 1.1)}px; color: ${p.text}; }
.hk-key.hk-bar { background-color: transparent; box-shadow: none; font-size: ${Math.round(font * 0.62)}px;
  font-weight: normal; color: ${p.text}; }
.hk-key.hk-bar:active { background-color: ${p.pressed}; }
.hk-key.hk-bar:checked { background-color: -st-accent-color; color: #ffffff; }
.hk-key.hk-bar.hk-locked { background-color: -st-accent-color; color: #ffffff; border: 2px solid #ffffff; }
.hk-key.hk-bar StIcon { icon-size: ${Math.round(font * 0.85)}px; }
.hk-key.hk-suggestion { font-size: ${Math.round(font * 0.78)}px; }
.hk-preview { background-color: ${p.pop}; color: ${p.text}; border-radius: ${radius + 4}px;
  font-size: ${font * 2}px; font-weight: bold; box-shadow: 0 2px 6px 0 rgba(0, 0, 0, 0.4); }
.hk-popup { background-color: ${p.pop}; border-radius: ${radius + 4}px; padding: 4px; spacing: 2px;
  box-shadow: 0 2px 6px 0 rgba(0, 0, 0, 0.4); }
.hk-popup-item { color: ${p.text}; font-size: ${Math.round(font * 1.25)}px; border-radius: ${radius}px; }
.hk-popup-item:selected { background-color: -st-accent-color; color: #ffffff; }
`;
    }

    // write and load the stylesheet if it changed (or always, with @force: the theme dropped it)
    update(force = false) {
        const css = this.css();
        if (!force && css === this._css && this._file)
            return;
        this._css = css;
        const theme = this._context.get_theme();
        this._loading = true;
        try {
            if (this._file)
                theme.unload_stylesheet(this._file);
            GLib.mkdir_with_parents(this._dir, 0o700);
            // a new name per load: St keeps a stylesheet it read under its file
            const path = GLib.build_filenamev([this._dir, `hkeyboard-${this._serial++ % 2}.css`]);
            GLib.file_set_contents(path, css);
            this._file = Gio.File.new_for_path(path);
            theme.load_stylesheet(this._file);
        } catch (e) {
            this._file = null;
            logError(e, 'gnome-phone: touch keyboard stylesheet');
        } finally {
            this._loading = false;
        }
    }

    destroy() {
        this._context.disconnectObject(this);
        if (this._file)
            this._context.get_theme().unload_stylesheet(this._file);
        this._file = null;
    }
}
