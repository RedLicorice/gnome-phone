// Hacker's Keyboard's layouts, as tools/hk2json.py converted them into hklayouts/<language>.json: the full 5-row
// keyboard and its Fn layer, plus the F1-F12 extension row. The xkb layout picks the language; English otherwise.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const LAYOUTS_DIR = GLib.uri_resolve_relative(import.meta.url, '../hklayouts', GLib.UriFlags.NONE);
const cache = new Map();

// xkb layout ('it', 'it+nodeadkeys', 'us') -> a language with a layout file
export function languageFor(group) {
    const base = (group ?? '').replace(/[+(].*/, '');
    return ['it'].includes(base) ? base : 'en';
}

export function load(language) {
    let data = cache.get(language);
    if (!data) {
        const file = Gio.File.new_for_uri(`${LAYOUTS_DIR}/${language}.json`);
        const [, contents] = file.load_contents(null);
        data = JSON.parse(new TextDecoder().decode(contents));
        cache.set(language, data);
    }
    return data;
}

// the compact layout's pages (letters, sym, sym2) for a language: hklayouts/compact.json, whose keys without a popup
// take the full layout's alternates for the same label (è é under e, the digits under the top row)
export function compactFor(language) {
    const id = `compact:${language}`;
    let pages = cache.get(id);
    if (!pages) {
        const full = load(language).layouts;
        const popups = new Map();
        for (const row of [...full.full, ...full.full_fn]) {
            for (const k of row.keys) {
                if (!k.code && k.label && k.popup && !popups.has(k.label))
                    popups.set(k.label, k.popup);
            }
        }
        const fill = k => !k.code && !k.spacer && !k.popup && popups.has(k.label) ? {...k, popup: popups.get(k.label)} : k;
        pages = Object.fromEntries(Object.entries(load('compact').layouts).map(([name, rows]) =>
            [name, rows.map(row => ({...row, keys: row.keys.map(fill)}))]));
        cache.set(id, pages);
    }
    return pages;
}

// the bar over the keys while no word is being typed: Hacker's controls a compact layout lacks
export const BAR_CONTROLS = [
    {code: 'escape', label: 'Esc'}, {code: 'tab'}, {code: 'ctrl', label: 'Ctrl'}, {code: 'alt', label: 'Alt'},
    {code: 'left', label: '◁', repeat: true}, {code: 'down', label: '▽', repeat: true},
    {code: 'up', label: '△', repeat: true}, {code: 'right', label: '▷', repeat: true},
    {code: 'emoji'}, {code: 'settings'}, {code: 'hide'},
];

// special keys: what they send (a keyval), or how they act
export const KEYVALS = {
    tab: Clutter.KEY_Tab, return: Clutter.KEY_Return, escape: Clutter.KEY_Escape, delete: Clutter.KEY_Delete,
    backspace: Clutter.KEY_BackSpace, home: Clutter.KEY_Home, end: Clutter.KEY_End,
    page_up: Clutter.KEY_Page_Up, page_down: Clutter.KEY_Page_Down, insert: Clutter.KEY_Insert,
    up: Clutter.KEY_Up, down: Clutter.KEY_Down, left: Clutter.KEY_Left, right: Clutter.KEY_Right,
    sysrq: Clutter.KEY_Sys_Req, scroll_lock: Clutter.KEY_Scroll_Lock, break: Clutter.KEY_Break,
    num_lock: Clutter.KEY_Num_Lock, caps_lock: Clutter.KEY_Caps_Lock,
    ...Object.fromEntries([...Array(12).keys()].map(i => [`f${i + 1}`, Clutter[`KEY_F${i + 1}`]])),
};

// sticky modifiers: what they hold down while the next key goes out
export const MODIFIERS = {
    shift: Clutter.KEY_Shift_L, ctrl: Clutter.KEY_Control_L, alt: Clutter.KEY_Alt_L, meta: Clutter.KEY_Super_L,
};

// keys whose selection mode (the ○ key) adds Shift
export const CURSOR = new Set(['up', 'down', 'left', 'right', 'home', 'end', 'page_up', 'page_down']);

export const ICONS = {
    backspace: 'osk-delete-symbolic', return: 'osk-enter-symbolic', shift: 'osk-shift-symbolic',
    menu: 'osk-emoji-picker-symbolic', settings: 'emblem-system-symbolic', emoji: 'osk-emoji-picker-symbolic',
    hide: 'osk-hide-symbolic', mode: 'input-keyboard-symbolic',
};

export const LABELS = {tab: '⇥', space: '', meta: '❖', fn: 'Fn'};
