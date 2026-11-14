// Interactive CPU boost hints for minerva-boost (docs/boost.md): what Android's PowerHAL does from the shell, where
// a tap or a launch starts from a low utilisation estimate and the first frames run on slow cores at a low clock.
// A touch sends Boost::INTERACTION, an app launch Mode::LAUNCH; the daemon owns the levels and the durations.
//
// The daemon is optional: the shell works without it. A hint is a fire-and-forget D-Bus call with NO_AUTO_START (the
// service must not be started by a hint), errors are ignored, and a missing daemon is logged once; hints go out
// regardless, so one that starts later is used at once.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const SERVICE = 'org.minerva.Boost1';
const PATH = '/org/minerva/Boost1';
const IFACE = 'org.minerva.Boost1';

// Boost::INTERACTION is throttled: a finger's TOUCH_BEGIN comes at most once per gesture, but a tap on the keyboard
// is a BUTTON_PRESS per key, and the hold extends on the daemon's side anyway
const TOUCH_MIN_US = 250000;

export class Boost {
    constructor() {
        this._touchAt = 0;
        this._loggedMissing = false;
        global.stage.connectObject('captured-event', (_stage, event) => this._onEvent(event), this);
    }

    // a finger came down anywhere: TOUCH_BEGIN on a touchscreen, BUTTON_PRESS from a pointer
    _onEvent(event) {
        switch (event.type()) {
        case Clutter.EventType.TOUCH_BEGIN:
        case Clutter.EventType.BUTTON_PRESS:
            this.touch();
            break;
        }
        return Clutter.EVENT_PROPAGATE;
    }

    touch() {
        const now = GLib.get_monotonic_time();
        if (now - this._touchAt < TOUCH_MIN_US)
            return;
        this._touchAt = now;
        this._hint('touch');
    }

    launch() {
        this._hint('launch');
    }

    _hint(kind) {
        try {
            Gio.DBus.session.call(SERVICE, PATH, IFACE, 'Hint', new GLib.Variant('(s)', [kind]), null,
                Gio.DBusCallFlags.NO_AUTO_START, -1, null, (bus, res) => {
                    try {
                        bus.call_finish(res);
                    } catch (e) {
                        if (this._loggedMissing)
                            return;
                        if (!e.matches(Gio.DBusError, Gio.DBusError.SERVICE_UNKNOWN) &&
                            !e.matches(Gio.DBusError, Gio.DBusError.NAME_HAS_NO_OWNER))
                            return;
                        this._loggedMissing = true;
                        log('gnome-phone: no minerva-boost on the session bus, CPU boost hints are not applied');
                    }
                });
        } catch (e) {
            // no session bus: the shell is not much use without it either
            logError(e, 'gnome-phone: CPU boost hint');
        }
    }

    destroy() {
        global.stage.disconnectObject(this);
    }
}
