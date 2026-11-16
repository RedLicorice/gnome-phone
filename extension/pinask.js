// The owner's PIN asked for by the Minerva voice service: the bouncer's PIN pad (bouncer.js PinView) on a sheet of its
// own over everything, checked by the same gp-auth as the lock screen's (keyguard.js checkPin), and the answer, right or
// not, given back over D-Bus (org.minerva.Pin1). The lock screen's own state is never touched: its throttle, its
// bouncer and its auto-confirm stay as they are, and the sheet is only read from it.
//
// The sheet is never shown over the lock screen, nor while its bouncer is up: with the phone locked the voice service
// gets no for an answer, and a phone that locks with the sheet up closes it at once (no) before the lock screen shows.
// What it says comes from the caller, already in the user's language.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {PinView} from './bouncer.js';
import {raiseToTop} from './endmark.js';
import {checkPin} from './keyguard.js';

const PIN_IFACE = `<node>
  <interface name="org.minerva.Pin1">
    <method name="AskPin">
      <arg name="reason" direction="in" type="s"/>
      <arg name="wrong_text" direction="in" type="s"/>
      <arg name="seconds" direction="in" type="i"/>
      <arg name="tries" direction="in" type="i"/>
      <arg name="ok" direction="out" type="b"/>
      <arg name="why" direction="out" type="s"/>
    </method>
    <method name="Cancel"/>
  </interface>
</node>`;

// a deadline of 0 or less is this one; none is longer than the last: a caller's bug must not leave an
// input-grabbing sheet up forever
const DEFAULT_SECONDS = 30;
const MAX_SECONDS = 600;
// wrong PINs a sheet takes before it gives up (no), unless the caller says otherwise for this ask
const DEFAULT_TRIES = 5;
// the backdrop's fade, as the view's own disappear (200 ms)
const FADE_MS = 200;

export class PinAsk {
    // @keyguard: .showing, .bouncerShowing and the 'changed' they come with, read to stay off the lock screen
    constructor(keyguard) {
        this._keyguard = keyguard;
        this._sheet = null;             // the open sheet: its widgets, its deadline, its promise's resolve
        this._closing = new Set();      // sheets still running their disappear
        this._export();
    }

    // The phone locked or on its way: the lock screen's, not the sheet's. Its PIN pad only with a PIN as the lock
    // type (a pattern or a swipe has no PIN to ask for), and not while the lock screen's own lockout runs, which the
    // sheet would otherwise get around.
    // '' when the sheet may open, else why not: the caller logs it, and answers the voice service with it
    _refused() {
        const kg = this._keyguard;
        if (!kg)
            return 'no-keyguard';
        if (kg.showing || kg.bouncerShowing || kg._bouncerActive)
            return 'locked';
        if (kg._lockType !== 'pin')
            return 'no-pin-lock';
        if (kg._lockedOut)
            return 'locked-out';
        return '';
    }

    // @reason: the message over the pad; @wrongText: the message after a wrong PIN (the reason when empty);
    // @seconds: the deadline; @tries: wrong PINs taken. Resolves true for the right PIN, false for anything else:
    // a wrong one too many times,
    // the deadline, a cancel, a tap off the pad, the phone locking, or a sheet already open
    // resolves [ok, why]: why is '' for the right PIN, else 'busy'|'locked'|'no-pin-lock'|'locked-out'|'no-keyguard'|
    // 'timeout'|'cancelled'|'wrong'|'check-failed'
    ask(reason, wrongText, seconds, tries) {
        if (this._sheet)
            return Promise.resolve([false, 'busy']);
        const why = this._refused();
        if (why)
            return Promise.resolve([false, why]);
        return new Promise(resolve => this._open(resolve, reason, wrongText, seconds, tries));
    }

    cancel() {
        if (this._sheet)
            this._finish(this._sheet, false, 'cancelled');
    }

    _open(resolve, reason, wrongText, seconds, tries) {
        const monitor = Main.layoutManager.primaryMonitor;
        const sheet = {
            resolve,
            reason: reason || '',
            wrongText: wrongText || reason || '',
            attempts: 0,
            tries: tries > 0 ? tries : DEFAULT_TRIES,
            checking: false,
            pressedOff: false,
            timeoutId: 0,
            grab: null,
        };
        // black over everything but the end mark, taking every touch: the pad is all there is while it is up
        sheet.backdrop = new St.Widget({
            name: 'gnomePhonePinAsk',
            style: 'background-color: rgba(0, 0, 0, 0.6);',
            reactive: true,
            opacity: 0,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
        });
        sheet.view = new PinView();
        sheet.view.set_position(Math.round((monitor.width - sheet.view.width) / 2),
            Math.round((monitor.height - sheet.view.height) / 2));
        sheet.backdrop.add_child(sheet.view);
        sheet.view.setMessage(sheet.reason, '');
        // the lock screen's auto-confirm length (set only for a PIN of 6 or more that unlocked), read, not changed
        sheet.view.autoConfirm = this._keyguard._pinLength ?? 0;
        sheet.view.connect('submit', (view, secret) => this._submit(sheet, secret, view.autoConfirmed));
        // a tap off the pad, pressed and released there, is a no; so is Escape
        sheet.backdrop.connect('captured-event', (actor, event) => this._offPad(sheet, event));
        sheet.backdrop.connect('key-press-event', (actor, event) => {
            if (event.get_key_symbol() !== Clutter.KEY_Escape)
                return Clutter.EVENT_PROPAGATE;
            this._finish(sheet, false, 'cancelled');
            return Clutter.EVENT_STOP;
        });

        Main.layoutManager.uiGroup.add_child(sheet.backdrop);
        raiseToTop(sheet.backdrop);
        sheet.grab = Main.pushModal(sheet.backdrop, {actionMode: Shell.ActionMode.SYSTEM_MODAL});
        global.stage.set_key_focus(sheet.backdrop);
        this._sheet = sheet;

        // the phone locking under the sheet: closed at once, so the lock screen is what shows
        this._keyguard.connectObject('changed', () => {
            if (this._keyguard.showing || this._keyguard._bouncerActive)
                this._finish(sheet, false, 'locked', false);
        }, this);
        const s = seconds > 0 ? Math.min(seconds, MAX_SECONDS) : DEFAULT_SECONDS;
        sheet.timeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, s, () => {
            sheet.timeoutId = 0;
            this._finish(sheet, false, 'timeout');
            return GLib.SOURCE_REMOVE;
        });

        sheet.backdrop.ease({opacity: 255, duration: FADE_MS, mode: Clutter.AnimationMode.EASE_OUT_QUAD});
        sheet.view.appear();
    }

    _offPad(sheet, event) {
        const type = event.type();
        const down = type === Clutter.EventType.BUTTON_PRESS || type === Clutter.EventType.TOUCH_BEGIN;
        const up = type === Clutter.EventType.BUTTON_RELEASE || type === Clutter.EventType.TOUCH_END;
        if (!down && !up)
            return Clutter.EVENT_PROPAGATE;
        const [x, y] = event.get_coords();
        const [ok, lx, ly] = sheet.view.transform_stage_point(x, y);
        const off = !ok || !sheet.view.ownsPoint(lx, ly);
        if (down) {
            sheet.pressedOff = off;
        } else if (off && sheet.pressedOff) {
            sheet.pressedOff = false;
            this._finish(sheet, false, 'cancelled');
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_PROPAGATE;
    }

    // as keyguard.js _submit, without its throttle: @secret null is any input (the message goes back to the reason),
    // 3 digits or fewer no attempt, a PIN gp-auth could not check a no
    async _submit(sheet, secret, auto) {
        if (this._sheet !== sheet || sheet.checking)
            return;
        if (secret === null) {
            sheet.view.setMessage(sheet.reason, '');
            return;
        }
        if (secret.length <= 3) {
            sheet.view.wrong();
            sheet.view.setEnabled(true);
            return;
        }
        sheet.checking = true;
        const result = await checkPin(secret);
        sheet.checking = false;
        // closed meanwhile (the deadline, a cancel, the lock): its answer was given
        if (this._sheet !== sheet)
            return;
        if (result === 0) {
            this._finish(sheet, true, '');
            return;
        }
        if (result !== 1) {
            this._finish(sheet, false, 'check-failed');
            return;
        }
        sheet.attempts++;
        // a PIN auto-confirm sent that was wrong: the PIN may have changed, enter takes one of another length
        if (auto)
            sheet.view.autoConfirm = 0;
        sheet.view.wrong();
        if (sheet.attempts >= sheet.tries) {
            this._finish(sheet, false, 'wrong');
            return;
        }
        sheet.view.setMessage(sheet.wrongText, '');
        sheet.view.setEnabled(true);
    }

    // Every way out comes here, once a sheet: the answer, the deadline gone, the grab and the 'changed' let go, and
    // the widgets out, after the view's disappear when @animated
    _finish(sheet, ok, why, animated = true) {
        if (this._sheet !== sheet)
            return;
        this._sheet = null;
        if (sheet.timeoutId)
            GLib.source_remove(sheet.timeoutId);
        sheet.timeoutId = 0;
        this._keyguard.disconnectObject(this);
        if (sheet.grab)
            Main.popModal(sheet.grab);
        sheet.grab = null;
        sheet.view.setEnabled(false);
        sheet.resolve([ok, why]);
        sheet.resolve = null;

        if (!animated || !St.Settings.get().enable_animations) {
            this._drop(sheet);
            return;
        }
        this._closing.add(sheet);
        sheet.backdrop.reactive = false;
        sheet.backdrop.ease({opacity: 0, duration: FADE_MS, mode: Clutter.AnimationMode.EASE_IN_QUAD});
        sheet.view.disappear(() => this._drop(sheet));
    }

    _drop(sheet) {
        this._closing.delete(sheet);
        if (!sheet.backdrop)
            return;
        // the view's own destroy() stops its animations, which its parent's destroy would not call
        sheet.view.destroy();
        sheet.backdrop.destroy();
        sheet.view = null;
        sheet.backdrop = null;
    }

    // --- org.minerva.Pin1

    _export() {
        this._dbus = Gio.DBusExportedObject.wrapJSObject(PIN_IFACE, this);
        this._dbus.export(Gio.DBus.session, '/org/minerva/Pin1');
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, 'org.minerva.Pin1',
            Gio.BusNameOwnerFlags.NONE, null, null);
    }

    // answered when the sheet closes (or at once, when it is refused), not when it opens
    AskPinAsync([reason, wrongText, seconds, tries], invocation) {
        this.ask(reason, wrongText, seconds, tries).catch(e => {
            logError(e, 'gnome-phone: AskPin');
            return [false, 'error'];
        }).then(([ok, why]) => invocation.return_value(new GLib.Variant('(bs)', [ok, why])));
    }

    Cancel() {
        this.cancel();
    }

    destroy() {
        // a call still waiting gets its no
        if (this._sheet)
            this._finish(this._sheet, false, 'closed', false);
        for (const sheet of [...this._closing])
            this._drop(sheet);
        if (this._nameId)
            Gio.bus_unown_name(this._nameId);
        this._nameId = 0;
        this._dbus?.unexport();
        this._dbus = null;
        this._keyguard = null;
    }
}
