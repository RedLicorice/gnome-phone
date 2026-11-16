// The screen on and off, as Android's PowerManager and doze do it: the power key, the double tap, the screen timeout,
// the always-on display, and the session's org.gnome.ScreenSaver.
//
// Awake, the screen shows the shell. Asleep, the lock screen is up behind a black layer over everything, and the
// panel is in its low power mode; with the AOD on (de.yesman.neo aod), that layer shows the AOD (aod.js) and the
// panel ticks a clock of its own. The display is never turned off: the S10e's DSI link cannot be brought back up
// once it went down, so mutter keeps the outputs on whatever gnome-settings-daemon asks of them
// (power-save-blocked, patches/mutter/0002) and "off" is a black frame at the panel's lowest brightness.
//
// org.gnome.ScreenSaver is active while asleep, as Phosh had it: s10e-sleepd suspends the phone then, and
// gnome-settings-daemon's blank, which follows it, is refused by mutter.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

import {Aod, Panel} from './aod.js';
import {HardwareKeys} from './keys.js';
import {animate, bezier, LINEAR} from './motion.js';

Gio._promisify(Gio.DBusProxy.prototype, 'call');

export const State = {AWAKE: 'awake', DOZING: 'dozing', ASLEEP: 'asleep'};

// going to sleep, the AOD (or black) fades in over the screen; waking from the AOD it fades out the same way, and from
// black the wake-up scrim fades over ANIMATION_DURATION_WAKEUP_SCRIM with a DecelerateInterpolator(1) (ScrimState.java)
const SLEEP_MS = 500;
const WAKE_FROM_AOD_MS = 500;
const WAKE_FROM_OFF_MS = 667;
const DECELERATE = t => 1 - (1 - t) * (1 - t);
// the lock screen's and the bouncer's own screen timeout (config_lockScreenDisplayTimeout,
// KeyguardViewMediator.AWAKE_INTERVAL_BOUNCER_MS)
const KEYGUARD_TIMEOUT_MS = 10000;
// a press of the power key held this long is a long press (the power menu): ViewConfiguration's
// getDeviceGlobalActionKeyTimeout
const LONG_PRESS_MS = 500;
// a sleep this soon after waking is taken as a bounce of the key
const MIN_AWAKE_MS = 300;
// gnome-session's inhibitor flag for idle (video playback, a presentation)
const INHIBIT_IDLE = 8;
// org.gnome.Mutter.DisplayConfig's PowerSaveMode (META_POWER_SAVE_ON, META_POWER_SAVE_OFF)
const POWER_SAVE_ON = 0;
const POWER_SAVE_OFF = 3;

const SCREENSAVER_IFACE = `<node>
  <interface name="org.gnome.ScreenSaver">
    <method name="Lock"/>
    <method name="GetActive"><arg name="active" direction="out" type="b"/></method>
    <method name="SetActive"><arg name="value" direction="in" type="b"/></method>
    <method name="GetActiveTime"><arg name="value" direction="out" type="u"/></method>
    <signal name="ActiveChanged"><arg name="new_value" type="b"/></signal>
    <signal name="WakeUpScreen"/>
  </interface>
</node>`;

// feedbackd's shortest tick, as the back gesture's
function haptic() {
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback',
        'TriggerFeedback', new GLib.Variant('(ssa{sv}i)', ['de.yesman.GnomePhone', 'button-released', {}, -1]),
        null, Gio.DBusCallFlags.NONE, -1, null, null);
}

function neoSettings() {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup('de.yesman.neo', true);
    return schema ? new Gio.Settings({settings_schema: schema}) : null;
}

export class Power extends Signals.EventEmitter {
    // @keyguard: lock() as the screen goes off, .showing, .bouncerShowing; @state: the SystemState; @menu: the power
    // menu a long press opens
    constructor(keyguard, state, menu, chord) {
        super();
        this._keyguard = keyguard;
        this._menu = menu;
        this._chord = chord;
        this._longPressId = 0;
        this._state = State.AWAKE;
        this._sleptAt = 0;
        this._wokeAt = GLib.get_monotonic_time();

        this._neo = neoSettings();
        this._session = new Gio.Settings({schema_id: 'org.gnome.desktop.session'});
        this._neo?.connectObject('changed', () => this._syncTimeout(), this);
        this._session.connectObject('changed::idle-delay', () => this._syncTimeout(), this);

        // mutter keeps the outputs on (see above)
        this._monitors = global.backend.get_monitor_manager();
        this._monitors.power_save_blocked = true;
        this._panel = new Panel();
        // whatever a shell that crashed asleep left behind
        this._panel.lowPower(false);
        this._panel.wakeGesture(false);

        // over everything, uiGroup's last: the AOD, or black
        const monitor = Main.layoutManager.primaryMonitor;
        this.layer = new St.Widget({
            name: 'gnomePhoneScreenOff',
            style: 'background-color: black;',
            reactive: true,
            visible: false,
            opacity: 0,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
        });
        this._aod = new Aod(this._panel, state);
        this.layer.add_child(this._aod);

        this._keys = new HardwareKeys({
            onPower: (pressed, time) => this._powerKey(pressed, time),
            onWake: () => this._doubleTap(),
        });

        this._idleMonitor = global.backend.get_core_idle_monitor();
        this._idleWatch = 0;
        this._inhibited = false;
        this._watchInhibitors();
        keyguard.connectObject('changed', () => this._syncTimeout(), this);

        this._exportScreenSaver();
        this._watchOutputs();
        this._syncTimeout();
    }

    get state() {
        return this._state;
    }

    get interactive() {
        return this._state === State.AWAKE;
    }

    // --- the keys

    _powerKey(pressed, time) {
        const now = GLib.get_monotonic_time();
        if (pressed) {
            this._pressedAt = now;
            // with volume down, a screenshot (chord.js); the press then does nothing more
            this._chord?.powerDown(this.interactive);
            // a press with the menu up closes it; the key then does what it would have
            this._menu?.closeNow();
            // Android wakes on the key's down (PhoneWindowManager.interceptPowerKeyDown)
            this._wokeByPress = !this.interactive;
            if (this._wokeByPress)
                this.wakeUp('power');
            // ... and held, opens the power menu (MSG_POWER_LONG_PRESS), with LONG_PRESS_POWER_BUTTON's tick
            this._cancelLongPress();
            this._longPressId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, LONG_PRESS_MS, () => {
                this._longPressId = 0;
                if (!this.interactive || this._chord?.powerTaken)
                    return GLib.SOURCE_REMOVE;
                haptic();
                this._menu?.open();
                return GLib.SOURCE_REMOVE;
            });
            return;
        }
        this._cancelLongPress();
        const held = (now - (this._pressedAt ?? now)) / 1000;
        this._pressedAt = null;
        const chord = this._chord?.powerUp();
        // ... and goes to sleep on its up, if the down did not wake it and the press was short
        if (chord || this._wokeByPress || held >= LONG_PRESS_MS)
            return;
        if ((now - this._wokeAt) / 1000 < MIN_AWAKE_MS)
            return;
        this.goToSleep('power');
    }

    _cancelLongPress() {
        if (this._longPressId)
            GLib.source_remove(this._longPressId);
        this._longPressId = 0;
    }

    _doubleTap() {
        if (!this.interactive && this._neo?.get_boolean('double-tap-wake') !== false)
            this.wakeUp('double-tap');
    }

    // --- going to sleep and waking

    get _aodEnabled() {
        return this._neo?.get_boolean('aod') ?? false;
    }

    goToSleep(reason) {
        if (!this.interactive)
            return;
        log(`gnome-phone: going to sleep (${reason})`);
        this._state = this._aodEnabled ? State.DOZING : State.ASLEEP;
        this._sleptAt = GLib.get_monotonic_time();
        this._menu?.closeNow();
        this._keyguard.lock();
        this._keyguard.asleep?.();

        this._aod.visible = this._state === State.DOZING;
        this._aod.prepare();
        this._showLayer();
        this._fade(255, SLEEP_MS, LINEAR, () => {
            // all black or all AOD on the panel: its low power mode, and with it the clock
            if (this._state === State.DOZING) {
                this._aod.shown();
            } else {
                this._panel.lowPower(true);
                this._outputsOff(true);
            }
            this._panel.wakeGesture(this._neo?.get_boolean('double-tap-wake') !== false);
        });
        this._syncTimeout();
        this._setActive(true);
        this.emit('state-changed');
    }

    wakeUp(reason) {
        if (this.interactive)
            return;
        log(`gnome-phone: waking up (${reason})`);
        const from = this._state;
        this._state = State.AWAKE;
        this._wokeAt = GLib.get_monotonic_time();
        // out of the low power mode (or off) before anything shows through
        this._outputsOff(false);
        this._panel.wakeGesture(false);
        this._aod.hidden();
        this._panel.lowPower(false);
        this._keyguard.wokeUp?.(reason);
        if (from === State.DOZING)
            this._fade(0, WAKE_FROM_AOD_MS, bezier(0.4, 0, 0.2, 1), () => this._hideLayer());
        else
            this._fade(0, WAKE_FROM_OFF_MS, DECELERATE, () => this._hideLayer());
        this._syncTimeout();
        this._setActive(false);
        this._emitScreenSaver('WakeUpScreen');
        this.emit('state-changed');
    }

    // the layer's fade; a fade the other way picks up from where this one got to
    _fade(to, duration, interpolator, onDone) {
        this._fadeAnim?.stop();
        const from = this.layer.opacity;
        if (!St.Settings.get().enable_animations)
            duration = 0;
        this._fadeAnim = animate(this.layer, {
            from, to, duration: duration * Math.abs(to - from) / 255, interpolator,
            onUpdate: v => {
                this.layer.opacity = Math.round(v);
            },
            onDone: () => {
                this._fadeAnim = null;
                onDone?.();
            },
        });
    }

    _showLayer() {
        this.layer.show();
        // touches on a dark screen go nowhere, and what is under it stops taking them
        if (!this._grab)
            this._grab = Main.pushModal(this.layer, {actionMode: Main.actionMode});
    }

    _hideLayer() {
        this.layer.hide();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
    }

    // --- the screen timeout: the lock screen's own, else the session's idle delay (0: never) unless an app inhibits
    // idle; after any input

    _syncTimeout() {
        if (this._idleWatch)
            this._idleMonitor.remove_watch(this._idleWatch);
        this._idleWatch = 0;
        if (!this.interactive)
            return;
        let ms;
        if (this._keyguard.showing)
            ms = KEYGUARD_TIMEOUT_MS;
        else if (!this._inhibited)
            ms = this._session.get_uint('idle-delay') * 1000;
        if (!ms)
            return;
        // the idle watch counts from the last input: a screen woken without one (a call) long after it gets its whole
        // timeout from now
        const idle = this._idleMonitor.get_idletime();
        // an idle watch stays after it fires (mutter removes only user-active ones): one left behind slept the phone
        // 10 s into every idle spell, unlocked, ever after the lock screen had once timed out
        const id = this._idleMonitor.add_idle_watch(idle > ms - 1000 ? idle + ms : ms, () => {
            this._idleMonitor.remove_watch(id);
            if (this._idleWatch === id)
                this._idleWatch = 0;
            this.goToSleep('timeout');
        });
        this._idleWatch = id;
    }

    _watchInhibitors() {
        this._sessionManager = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION, Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES,
            null, 'org.gnome.SessionManager', '/org/gnome/SessionManager', 'org.gnome.SessionManager', null);
        this._sessionManager.connectObject('g-signal', (proxy, sender, signal) => {
            if (signal === 'InhibitorAdded' || signal === 'InhibitorRemoved')
                this._checkInhibited();
        }, this);
        this._checkInhibited();
    }

    async _checkInhibited() {
        try {
            const [inhibited] = (await this._sessionManager.call('IsInhibited',
                new GLib.Variant('(u)', [INHIBIT_IDLE]), Gio.DBusCallFlags.NONE, 2000, null)).deepUnpack();
            if (inhibited !== this._inhibited) {
                this._inhibited = inhibited;
                this._syncTimeout();
            }
        } catch (e) {
            logError(e, 'gnome-phone: idle inhibitors');
        }
    }

    // --- org.gnome.ScreenSaver: active while the screen is asleep

    _exportScreenSaver() {
        // Debian's GNOME Shell exports its screen shield's own object here (shellDBus.js ScreenSaverDBus), and a
        // second export fails: the extension stopped half built, with no keyboard and its sleep half running. The
        // phone's sleep is ours, so the shell's object steps aside while the extension runs and comes back after.
        const shell = Main.screenShield?._screenSaverDBus?._dbusImpl ?? null;
        if (shell?.get_object_path?.()) {
            shell.unexport();
            this._shellScreenSaver = shell;
        }
        this._dbus = Gio.DBusExportedObject.wrapJSObject(SCREENSAVER_IFACE, this);
        this._dbus.export(Gio.DBus.session, '/org/gnome/ScreenSaver');
        this._nameId = Gio.bus_own_name_on_connection(Gio.DBus.session, 'org.gnome.ScreenSaver',
            Gio.BusNameOwnerFlags.REPLACE, null, null);
        this._active = false;
    }

    // Black on a panel without a low power mode (the POCO's) is still a lit panel: it glowed until the phone suspended
    // a few seconds later (owner, 2026-11-12: "closes the screen but does not switch it off ... glows black"). There
    // the outputs go off for real once the black is up, as gnome-settings-daemon would have them: power_save_blocked
    // comes off for that, and back on before they are turned on again (an "on" goes through it anyway). A panel with
    // a low power mode (the S10e's, whose DSI link does not come back from off) stays on, at its lowest brightness.
    _outputsOff(off) {
        if (this._panel.hasLowPower || off === (this._outputsAreOff ?? false))
            return;
        this._outputsAreOff = off;
        if (off)
            this._displayArmed = false;
        this._monitors.power_save_blocked = !off;
        Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
            'org.freedesktop.DBus.Properties', 'Set',
            new GLib.Variant('(ssv)', ['org.gnome.Mutter.DisplayConfig', 'PowerSaveMode',
                new GLib.Variant('i', off ? POWER_SAVE_OFF : POWER_SAVE_ON)]),
            null, Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
                try {
                    bus.call_finish(res);
                    // the displays took it: a panel that comes back on from here is not our doing
                    this._displayArmed = off && !this._destroyed;
                } catch (e) {
                    // the panel is lit while this side believes it asleep: the state the watcher above is for
                    log(`gnome-phone: outputs ${off ? 'off' : 'on'}: ${e.message}`);
                }
            });
    }

    _setActive(active) {
        if (active === this._active)
            return;
        this._active = active;
        this._dbus?.emit_signal('ActiveChanged', new GLib.Variant('(b)', [active]));
    }

    // The outputs' own state, read back from mutter. Asleep, this side lets them power save (_outputsOff: the displays
    // unblocked, PowerSaveMode off) and mutter brings them back on the first input it sees; the panel then lights while
    // the phone is still asleep here, the black layer stays up, and the face behind it never wakes (the kiosk's, in
    // Giuseppe's report: a lit screen with nothing on it). An output that comes back on is a wake, whatever woke it.
    //
    // Only once our own "off" has gone through (_displayArmed): the events of the calls before it are still on their
    // way, and a panel that reports itself on because it cannot power save would otherwise undo every sleep at once.
    _watchOutputs() {
        this._displayArmed = false;
        this._outputsId = Gio.DBus.session.signal_subscribe('org.gnome.Mutter.DisplayConfig',
            'org.freedesktop.DBus.Properties', 'PropertiesChanged', '/org/gnome/Mutter/DisplayConfig', null,
            Gio.DBusSignalFlags.NONE, (_c, _s, _p, _i, _sig, params) => {
                if (this._destroyed || this._panel.hasLowPower)
                    return;
                let value;
                try {
                    const [iface, changed] = params.deepUnpack();
                    if (iface !== 'org.gnome.Mutter.DisplayConfig')
                        return;
                    value = changed.PowerSaveMode;
                    if (value?.deepUnpack)
                        value = value.deepUnpack();
                } catch (e) {
                    return;
                }
                if (value === POWER_SAVE_OFF) {
                    this._displayArmed = true;
                    return;
                }
                if (value === POWER_SAVE_ON && this._displayArmed && !this.interactive)
                    this.wakeUp('display');
            });
    }

    _emitScreenSaver(signal) {
        this._dbus?.emit_signal(signal, null);
    }

    Lock() {
        this.goToSleep('lock');
    }

    GetActive() {
        return this._active;
    }

    SetActive(value) {
        if (value)
            this.goToSleep('screensaver');
        else
            this.wakeUp('screensaver');
    }

    GetActiveTime() {
        return this._active ? Math.floor((GLib.get_monotonic_time() - this._sleptAt) / 1e6) : 0;
    }

    destroy() {
        this._destroyed = true;
        this._cancelLongPress();
        this._fadeAnim?.stop();
        if (this._idleWatch)
            this._idleMonitor.remove_watch(this._idleWatch);
        this._sessionManager?.disconnectObject(this);
        this._keyguard.disconnectObject(this);
        this._keys.destroy();
        if (this._outputsId)
            Gio.DBus.session.signal_unsubscribe(this._outputsId);
        this._outputsId = 0;
        if (this._nameId)
            Gio.bus_unown_name(this._nameId);
        this._dbus?.unexport();
        this._dbus = null;
        this._shellScreenSaver?.export(Gio.DBus.session, '/org/gnome/ScreenSaver');
        this._shellScreenSaver = null;
        this._panel.wakeGesture(false);
        this._panel.lowPower(false);
        this._outputsOff(false);
        this._aod.destroy();
        this._hideLayer();
        this.layer.destroy();
        this._monitors.power_save_blocked = false;
        this._neo?.disconnectObject(this);
        this._session.disconnectObject(this);
    }
}
