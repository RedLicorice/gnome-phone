// The touch keyboard: Hacker's Keyboard's full layout and behaviour (hkeyboard/view.js) in place of the keys of
// GNOME Shell's on-screen keyboard. The shell's Keyboard stays what it is (open and close, the slide, focus tracking,
// the input method, the emoji page, the suggestions, Android's keyboard requests through osk.js): only the keys it
// builds are replaced, in Keyboard._updateKeys, and the keypads for digits, numbers and phone numbers stay the shell's.
// The keyboard also opens at once (the shell waits 300 ms first, KEYBOARD_REST_TIME) and takes its height from the
// settings (de.yesman.GnomePhone.Keyboard, Neo Settings' "Touch keyboard" page), which apply live.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as InputSourceManager from 'resource:///org/gnome/shell/ui/status/keyboard.js';

import {languageFor, load} from './hkeyboard/layout.js';
import {KeyboardStyle} from './hkeyboard/style.js';
import {KeyboardView} from './hkeyboard/view.js';
import {openSettings} from './settings.js';

const SCHEMA = 'de.yesman.GnomePhone.Keyboard';
const STOCK_PURPOSES = new Set([
    Clutter.InputContentPurpose.DIGITS, Clutter.InputContentPurpose.NUMBER, Clutter.InputContentPurpose.PHONE,
]);
// settings that change the rows, or only the keys' placement
const REBUILD = new Set(['extension-row', 'top-row-scale']);
const HEIGHT = new Set(['height-portrait', 'height-landscape']);

export class TouchKeyboard {
    constructor() {
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup(SCHEMA, true);
        if (!schema) {
            console.warn(`gnome-phone: ${SCHEMA} is not installed, the shell's keyboard stays`);
            return;
        }
        this._settings = new Gio.Settings({settings_schema: schema});
        this._style = new KeyboardStyle(this._settings, () => this._rowHeight());

        const self = this;
        const proto = KeyboardUI.Keyboard.prototype;
        const orig = this._orig = {
            _updateKeys: proto._updateKeys, _setActiveLevel: proto._setActiveLevel,
            _relayout: proto._relayout, open: proto.open,
        };
        proto._updateKeys = function () {
            if (self._wanted(this)) {
                try {
                    self._attach(this);
                    return;
                } catch (e) {
                    logError(e, 'gnome-phone: the touch keyboard, the shell\'s keys instead');
                }
            }
            self._detach(this);
            orig._updateKeys.call(this);
        };
        proto._setActiveLevel = function (level) {
            const view = this._hkView;
            if (view && this._currentPage === view)
                view.setAutoShift(level === 'shift');
            else
                orig._setActiveLevel.call(this, level);
        };
        proto._relayout = function () {
            if (!self._settings.get_boolean('enabled'))
                return orig._relayout.call(this);
            const monitor = Main.layoutManager.keyboardMonitor;
            if (!monitor)
                return undefined;
            this.width = monitor.width;
            this.height = Math.round(monitor.height * self._heightPercent(monitor) / 100);
            return undefined;
        };
        proto.open = function (immediate) {
            return orig.open.call(this, self._settings.get_boolean('enabled') ? true : immediate);
        };

        this._settings.connectObject('changed', (_s, key) => this._changed(key), this);
        // the keys are first built once the extension is up: a fault in them leaves the shell's keys, never the
        // rest of the extension half enabled
        this._refreshId = GLib.idle_add_once(GLib.PRIORITY_DEFAULT, () => {
            this._refreshId = 0;
            try {
                this._refresh();
            } catch (e) {
                logError(e, 'gnome-phone: the touch keyboard, the shell\'s keys instead');
                this.destroy();
            }
        });
    }

    get _keyboard() {
        return Main.keyboard._keyboard ?? null;
    }

    _wanted(keyboard) {
        return this._settings.get_boolean('enabled') && !STOCK_PURPOSES.has(keyboard._keyboardController.purpose);
    }

    _heightPercent(monitor) {
        return this._settings.get_int(monitor.width > monitor.height ? 'height-landscape' : 'height-portrait');
    }

    // a row's height for the labels' size: the keyboard's height over its rows
    _rowHeight() {
        const monitor = Main.layoutManager.keyboardMonitor ?? Main.layoutManager.primaryMonitor;
        if (!monitor)
            return 40;
        const units = this._keyboard?._hkView?.rowUnits() ?? 5;
        return monitor.height * this._heightPercent(monitor) / 100 / units;
    }

    // our keys as the keyboard's current page, made once per language and kept across fields
    _attach(keyboard) {
        const language = languageFor(keyboard._keyboardController.getCurrentGroup());
        let view = keyboard._hkView;
        if (view?.language !== language) {
            view?.destroy();
            view = new KeyboardView(load(language), this._settings);
            view.language = language;
            view.connect('action', (_v, action) => this._action(keyboard, action));
            view.connect('destroy', () => {
                if (keyboard._hkView === view)
                    keyboard._hkView = null;
                if (keyboard._currentPage === view)
                    keyboard._currentPage = null;
            });
            keyboard._aspectContainer.add_child(view);
            keyboard._hkView = view;
        }
        view.controller = keyboard._keyboardController;
        view.keyboard = keyboard;

        // the shell's keys, if a keypad was up
        if (keyboard._currentPage && keyboard._currentPage !== view) {
            keyboard._currentPage.disconnect(keyboard._currentPage._destroyID);
            delete keyboard._currentPage._destroyID;
        }
        keyboard._currentLayout?.destroy();
        keyboard._currentLayout = null;

        keyboard._layers = {default: view, shift: view};
        keyboard._emojiVisible = keyboard._shouldShowEmoji();
        keyboard._modifiers.clear();
        keyboard._modifierKeys.clear();
        keyboard._currentPage = view;
        view.show();
        keyboard._updateCurrentPageVisible();
        // a ratio that never narrows the keys (AspectContainer centers a page narrower than the keyboard)
        keyboard._aspectContainer.setRatio(1000, 1);
        keyboard._emojiSelection.setRatio(10, 4);
        view.setAutoShift(false);
        this._style.update();
    }

    _detach(keyboard) {
        const view = keyboard._hkView;
        if (!view)
            return;
        if (keyboard._currentPage === view)
            keyboard._currentPage = null;
        keyboard._latched = false;
        view.hide();
    }

    _action(keyboard, action) {
        switch (action) {
        case 'close':
            keyboard.close(true);
            break;
        case 'extension':
            this._settings.set_boolean('extension-row', !this._settings.get_boolean('extension-row'));
            break;
        case 'settings':
            keyboard.close(true);
            openSettings('touch-keyboard');
            break;
        case 'height-up':
        case 'height-down': {
            const monitor = Main.layoutManager.keyboardMonitor;
            const key = monitor && monitor.width > monitor.height ? 'height-landscape' : 'height-portrait';
            const value = this._settings.get_int(key) + (action === 'height-up' ? 5 : -5);
            this._settings.set_int(key, Math.clamp(value, 15, 75));
            break;
        }
        case 'lang-prev':
        case 'lang-next': {
            const manager = InputSourceManager.getInputSourceManager();
            const sources = manager.inputSources;
            const n = Object.keys(sources).length;
            const current = manager.currentSource?.index ?? 0;
            if (n > 1)
                sources[(current + (action === 'lang-next' ? 1 : -1) + n) % n]?.activate(true);
            break;
        }
        case 'emoji':
            keyboard._toggleEmoji();
            break;
        }
    }

    _changed(key) {
        if (key === 'enabled') {
            this._refresh();
            return;
        }
        const view = this._keyboard?._hkView;
        if (REBUILD.has(key))
            view?.rebuild();
        else
            view?.queue_relayout();
        if (HEIGHT.has(key) || REBUILD.has(key))
            this._keyboard?._relayout();
        this._style.update();
    }

    // the live keyboard made again with the keys and height as the settings now want them
    _refresh() {
        const keyboard = this._keyboard;
        if (!keyboard)
            return;
        keyboard._updateKeys();
        keyboard._relayout();
    }

    destroy() {
        if (this._refreshId)
            GLib.source_remove(this._refreshId);
        this._refreshId = 0;
        if (!this._settings)
            return;
        this._settings.disconnectObject(this);
        Object.assign(KeyboardUI.Keyboard.prototype, this._orig);
        const keyboard = this._keyboard;
        if (keyboard?._hkView) {
            this._detach(keyboard);
            keyboard._hkView.destroy();
            keyboard._updateKeys();
            keyboard._relayout();
        }
        this._style.destroy();
        this._settings = null;
    }
}
