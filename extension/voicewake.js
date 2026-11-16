// A recognized voice, from the Minerva voice service, may wake the screen (org.minerva.ScreenWake1): the lock screen
// shows with the kiosk face over it, as power.js wakeUp leaves it, and the phone stays locked. When it may is the lock
// screen's own policy (lockover.js voiceWakeAllowed, pure and tested under plain gjs): the PIN entered since boot or a
// shell restart first, not in Lockdown, not in the PIN's own timeout.
import Gio from 'gi://Gio';

import {voiceWakeAllowed} from './lockover.js';

const WAKE_IFACE = `<node>
  <interface name="org.minerva.ScreenWake1">
    <method name="Check">
      <arg name="allowed" direction="out" type="b"/>
      <arg name="why" direction="out" type="s"/>
    </method>
    <method name="Wake">
      <arg name="person" direction="in" type="s"/>
      <arg name="woke" direction="out" type="b"/>
      <arg name="why" direction="out" type="s"/>
    </method>
  </interface>
</node>`;

export class VoiceWake {
    // @keyguard: _secure, _afterRestart, _lockedDown and _lockedOut, read, never changed; @power: .interactive (the
    // screen on) and wakeUp(reason)
    constructor({keyguard, power}) {
        this._keyguard = keyguard;
        this._power = power;
        this._export();
    }

    // '' when a voice may wake the screen now, else why: 'pin-not-entered'|'locked-down'|'locked-out'|'no-keyguard'
    _refused() {
        const kg = this._keyguard;
        if (!kg)
            return 'no-keyguard';
        return voiceWakeAllowed({
            secure: kg._secure,
            afterRestart: kg._afterRestart,
            lockedDown: kg._lockedDown,
            lockedOut: kg._lockedOut,
        });
    }

    // --- org.minerva.ScreenWake1

    Check() {
        const why = this._refused();
        return [why === '', why];
    }

    Wake(person) {
        if (this._power?.interactive)
            return this._answer(person, true, 'awake');
        const why = this._refused();
        if (why)
            return this._answer(person, false, why);
        this._power?.wakeUp('voice');
        return this._answer(person, true, '');
    }

    // one line each way: the owner reads the journal after a phone that would not wake to a voice
    _answer(person, woke, why) {
        log(`gnome-phone: voice wake (${person}): ${woke ? 'woke' : why}`);
        return [woke, why];
    }

    _export() {
        this._dbus = Gio.DBusExportedObject.wrapJSObject(WAKE_IFACE, this);
        this._dbus.export(Gio.DBus.session, '/org/minerva/ScreenWake1');
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, 'org.minerva.ScreenWake1',
            Gio.BusNameOwnerFlags.NONE, null, null);
    }

    destroy() {
        if (this._nameId)
            Gio.bus_unown_name(this._nameId);
        this._nameId = 0;
        this._dbus?.unexport();
        this._dbus = null;
        this._keyguard = null;
        this._power = null;
    }
}
