// A text field that a tap gives focus opens the keyboard, as on Android (InputMethodManager.showSoftInput from the
// view the finger touched). GTK asks for the keyboard itself when its own text widget is tapped
// (gtk_im_context_activate_osk), but a WebKit page's fields never ask (a site's password box stayed without keys), and
// neither does a field an app focuses in answer to a tap elsewhere (the browser's address bar opened by its pill):
// their text input is enabled without show_input_panel, so mutter keeps the keyboard closed.
//
// So: a finger comes down while no client text field has the input focus; within FOCUS_WAIT_MS of it lifting, one
// does → the keyboard opens, on that window's monitor. The input method's focus is polled for that short while only
// (there is no signal for it): nothing runs between touches. A field that keeps the focus across the tap (a key on the
// keyboard, the send button) opens nothing, and the focus leaving the field closes the keyboard as before (the input
// method's own focus-out).
//
// The keyboard goes the moment the tap closes it, as on Android. The window under it is another matter: GTK moves the
// focus on a button's PRESS (focus-on-click), and the window the keyboard had shrunk grows back at once, so a dialog's
// Cancel or Rename touched while the keyboard was up moved down the screen before the finger lifted: the release
// missed the button and the dialog stayed, re-centred lower, as if it had opened again (neo-settings' phone name,
// 2026-11-12). So the shell's slide down runs at once, and only the window moving back waits for the lift: at most
// close-window-hold-ms after the press, for a lift that never came.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const SCHEMA = 'de.yesman.GnomePhone.Keyboard';
const FOCUS_WAIT_MS = 800;      // a page's field: the tap goes to WebKit's web process and the focus comes back
const POLL_MS = 40;
const WINDOW_HOLD_MS = 400;     // a window's way back, while a finger is down (close-window-hold-ms in the schema)

export class TouchFocusKeyboard {
    constructor() {
        this._hadFocus = true;
        this._pollId = 0;
        this._down = false;
        this._held = null;      // a window's way back the shell asked for while a finger was down
        this._heldId = 0;
        this._settings = null;
        try {
            const schema = Gio.SettingsSchemaSource.get_default()?.lookup(SCHEMA, true);
            if (schema)
                this._settings = new Gio.Settings({settings_schema: schema});
        } catch (e) {
            logError(e, 'gnome-phone: the keyboard\'s settings, the times below stay');
        }
        this._holdWindow();
        global.stage.connectObject('captured-event', (_stage, event) => this._onEvent(event), this);
    }

    get _holdMs() {
        try {
            return this._settings?.get_int('close-window-hold-ms') ?? WINDOW_HOLD_MS;
        } catch {
            return WINDOW_HOLD_MS;  // a schema older than this key: the keyboard still closes
        }
    }

    // the shell's move of the window back under the keys, held while a finger is down: the keyboard closes at once,
    // the window keeps its place under the finger (the class, not the instance: the keyboard is made again whenever
    // the OSK setting changes)
    _holdWindow() {
        const proto = KeyboardUI.Keyboard?.prototype;
        if (!proto?._animateWindow || !proto?.open)
            return;
        const animateWindow = proto._animateWindow;
        const open = proto.open;
        const self = this;
        proto._animateWindow = function (window, show) {
            if (!show && self._down) {
                self._hold(this, window, animateWindow);
                return undefined;
            }
            return animateWindow.call(this, window, show);
        };
        proto.open = function (...args) {
            self._drop();
            return open.apply(this, args);
        };
        this._unhold = () => {
            proto._animateWindow = animateWindow;
            proto.open = open;
        };
    }

    _hold(keyboard, window, animateWindow) {
        // a window already waiting goes back first: this one is the finger's now
        this._release();
        this._held = {keyboard, window, animateWindow};
        this._heldId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._holdMs, () => {
            this._heldId = 0;
            this._down = false;     // the lift was not ours to see: nothing may wait for it again
            this._release();
            return GLib.SOURCE_REMOVE;
        });
        GLib.Source.set_name_by_id(this._heldId, '[gnome-phone] the window back under the keys');
    }

    // the window back the moment the finger lifts
    _release() {
        if (this._heldId)
            GLib.source_remove(this._heldId);
        this._heldId = 0;
        const held = this._held;
        if (!held)
            return;
        this._held = null;
        try {
            held.animateWindow.call(held.keyboard, held.window, false);
        } catch (e) {
            logError(e, 'gnome-phone: the window back from the keyboard');
        }
    }

    // the keyboard opens again: the window goes up with it, not back
    _drop() {
        if (this._heldId)
            GLib.source_remove(this._heldId);
        this._heldId = 0;
        this._held = null;
    }

    _onEvent(event) {
        const type = event.type();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            this._down = true;
            this._hadFocus = Main.inputMethod.currentFocus !== null;
        } else if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.TOUCH_CANCEL) {
            this._down = false;
            this._release();
            if (type === Clutter.EventType.TOUCH_END)
                this._watch();
        }
        return Clutter.EVENT_PROPAGATE;
    }

    _watch() {
        this._stop();
        const end = GLib.get_monotonic_time() + FOCUS_WAIT_MS * 1000;
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, POLL_MS, () => {
            if (Main.inputMethod.currentFocus === null) {
                // the focus left a field: it may come back to another one
                this._hadFocus = false;
            } else if (!this._hadFocus) {
                this._pollId = 0;
                this._open();
                return GLib.SOURCE_REMOVE;
            }
            if (GLib.get_monotonic_time() < end)
                return GLib.SOURCE_CONTINUE;
            this._pollId = 0;
            return GLib.SOURCE_REMOVE;
        });
        GLib.Source.set_name_by_id(this._pollId, '[gnome-phone] touch focus keyboard');
    }

    _open() {
        if (Main.sessionMode.isLocked || Main.keyboard.visible)
            return;
        const win = global.display.focus_window;
        Main.keyboard.open(win ? win.get_monitor() : Main.layoutManager.primaryIndex);
    }

    _stop() {
        if (this._pollId)
            GLib.source_remove(this._pollId);
        this._pollId = 0;
    }

    destroy() {
        this._stop();
        global.stage.disconnectObject(this);
        this._release();
        this._unhold?.();
    }
}
