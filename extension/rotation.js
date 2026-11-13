// A phone turns its apps, not itself: the lock screen is portrait (config_enableLockScreenRotation is false) and so is
// the home screen (Launcher3's "Allow home screen rotation", off by default on a phone). With GNOME's orientation lock
// (Android's auto-rotate) off, the panel follows the phone only while an app is in front; locked, at home or in the app
// drawer it is upright, and an app going away turns it back. An app can also have an orientation of its own, as an
// Android app's manifest gives it: X-Phone-Orientation=landscape in its desktop file (a game) keeps the panel sideways
// while it is in front, however the phone is held, or lies on the table; X-Phone-Orientation=portrait keeps it
// upright (an app laid out for portrait only). Mutter's orientation manager turns the panel with the accelerometer;
// while the panel is to stay upright or sideways its tracking is held and the panel set with a temporary monitor
// config, as mutter applies the sensor's own. The shell's UI is laid out for an upright panel too, so a shell started
// sideways turns before it is built (whenUpright). Docked (another monitor on) nothing is turned.
//
// Holding the tracking needs meta_orientation_manager_inhibit_tracking(), which a stock mutter has in its typelib but
// does not export (gnome-phone's mutter patch 0004 does). GJS calling it there does not throw: it aborts the shell (an
// assertion in its function invoker), so the symbol is looked up first. Without it the panel is held with GNOME's
// orientation lock
// instead: set while held, and given back as the user had it when let go. The user's own setting is kept in a file
// meanwhile, so a shell that dies holding the lock gives it back as it starts again, and Android's Auto-rotate
// (state.js) reads and sets the user's setting, not the hold.
import Gio from 'gi://Gio';
import GIRepository from 'gi://GIRepository?version=3.0';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const NORMAL = 0;               // MtkMonitorTransform
const SEASCAPE = 1;             // 90: the phone turned right, its left side up
const LANDSCAPE = 3;            // 270: turned left, its right side up (Android's landscape, ROTATION_90)
const FOLLOW = -1;              // not a transform: the panel follows the phone
const TEMPORARY = 1;            // MetaMonitorsConfigMethod
// mutter claims the sensor again when let go and turns the panel only on a change of its reading: held as it was
// before, the phone would stay upright. Its reading is fresh by then.
const RESYNC_MS = 700;
// a shell started sideways is built anyway if the panel has not turned by then
const UPRIGHT_TIMEOUT_MS = 2000;

const STATE_TYPE = '(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})';

const LOCK_KEY = 'orientation-lock';
// the user's orientation lock while the panel is held with it: 'true' or 'false'
const SAVED_LOCK = GLib.build_filenamev([GLib.get_user_state_dir(), 'gnome-phone', 'orientation-lock']);

// The Rotations holding the panel with GNOME's orientation lock, and the user's own setting meanwhile. Two can hold
// at once (the extension turned on again before its last instance is gone): the first takes the user's setting, the
// last gives it back.
const lockHolders = new Set();
let userLock = false;

// mutter exports the tracking inhibit: both symbols found as GJS finds them before a call
function canInhibitTracking() {
    try {
        const info = GIRepository.Repository.dup_default().find_by_name('Meta', 'OrientationManager');
        const typelib = info?.get_typelib();
        return !!typelib && ['inhibit_tracking', 'uninhibit_tracking'].every(name => {
            const method = info.find_method(name);
            return !!method && typelib.symbol(method.get_symbol())[0];
        });
    } catch (e) {
        logError(e, 'gnome-phone: looking up mutter\'s orientation tracking inhibit');
        return false;
    }
}

function readSavedLock() {
    try {
        const [, bytes] = GLib.file_get_contents(SAVED_LOCK);
        const text = new TextDecoder().decode(bytes).trim();
        return text === 'true' ? true : text === 'false' ? false : null;
    } catch {
        return null;
    }
}

function writeSavedLock(locked) {
    try {
        GLib.mkdir_with_parents(GLib.path_get_dirname(SAVED_LOCK), 0o700);
        GLib.file_set_contents(SAVED_LOCK, locked ? 'true' : 'false');
    } catch (e) {
        logError(e, 'gnome-phone: saving the orientation lock');
    }
}

function dropSavedLock() {
    GLib.unlink(SAVED_LOCK);
}

// GNOME's orientation lock as the user set it (Android's auto-rotate, inverted), whether or not the panel is held
// with it now
export function userOrientationLock(settings) {
    return lockHolders.size ? userLock : settings.get_boolean(LOCK_KEY);
}

export function setUserOrientationLock(settings, locked) {
    if (lockHolders.size) {
        userLock = locked;
        writeSavedLock(locked);
    } else {
        settings.set_boolean(LOCK_KEY, locked);
    }
}

function displayConfig(method, params, type = null) {
    return new Promise((resolve, reject) => Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig',
        '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig', method, params,
        type ? new GLib.VariantType(type) : null, Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
            try {
                resolve(bus.call_finish(res));
            } catch (e) {
                reject(e);
            }
        }));
}

export class Rotation {
    constructor() {
        this._sensor = global.backend.get_orientation_manager();
        this._touchscreen = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.peripherals.touchscreen'});
        this._keyguard = null;
        this._home = null;
        this._tracker = Shell.WindowTracker.get_default();
        this._held = false;
        this._wanted = FOLLOW;          // FOLLOW or the transform the panel is held at
        this._forced = false;           // held sideways for an app
        this._busy = false;
        this._again = false;
        this._resyncId = 0;
        this._syncId = 0;
        this._waits = new Map();        // whenUpright's: id -> its timeout
        this._nextWait = 0;
        this._canInhibit = canInhibitTracking();    // else the orientation lock holds the panel
        // a shell that died holding the orientation lock: the user's own setting back first
        const saved = lockHolders.size ? null : readSavedLock();
        if (saved !== null) {
            this._touchscreen.set_boolean(LOCK_KEY, saved);
            dropSavedLock();
        }
        // mutter turned the panel itself: nothing left to resync
        this._sensor.connectObject('orientation-changed', () => this._cancelResync(), this);
        Main.layoutManager.connectObject('monitors-changed', () => this._monitorsChanged(), this);
    }

    // the panel upright, or not ours to turn
    get upright() {
        const monitors = Main.layoutManager.monitors;
        if (monitors.length !== 1)
            return true;
        return monitors[0].height >= monitors[0].width;
    }

    // Upright until this is called (the shell starts locked); then upright while the lock screen shows or no app is
    // in front of the launcher
    follow(keyguard, home) {
        this._keyguard = keyguard;
        this._home = home;
        keyguard.connectObject('changed', () => this._sync(), this);
        const queue = () => this._queueSync();
        global.display.connectObject('restacked', queue, 'notify::focus-window', queue, this);
        global.window_manager.connectObject('minimize', queue, 'unminimize', queue, 'map', queue,
            'destroy', queue, this);
        this._sync();
    }

    // the top app window over the launcher on this workspace (going home minimizes them all), as the status bar has
    // it; null at home
    _frontApp() {
        const workspace = global.workspace_manager.get_active_workspace();
        const windows = workspace.list_windows().filter(w =>
            !w.minimized && !this._home.isHome(w) && w.get_window_type() === Meta.WindowType.NORMAL);
        return global.display.sort_windows_by_stacking(windows).at(-1) ?? null;
    }

    // X-Phone-Orientation in the window's app's desktop file: landscape (a game), portrait (an app laid out for
    // portrait only, turning its icons itself as Aperture's nosensor activity does), or null to follow the phone
    _wantedOrientation(win) {
        const info = this._tracker.get_window_app(win)?.get_app_info();
        return info?.get_string('X-Phone-Orientation') ?? null;
    }

    // an idle, not a later before the next redraw: a fullscreen game is scanned out, and the shell may not redraw
    // until something of its own moves (a game opened sideways stayed upright)
    _queueSync() {
        if (this._syncId)
            return;
        this._syncId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._syncId = 0;
            this._sync();
            return GLib.SOURCE_REMOVE;
        });
    }

    _sync() {
        if (!this._keyguard)
            return;
        const front = this._keyguard.showing ? null : this._frontApp();
        const wanted = front ? this._wantedOrientation(front) : null;
        if (!front || wanted === 'portrait')
            this.hold();
        else if (wanted === 'landscape')
            this.holdSideways();
        else
            this.release();
    }

    // upright, whichever way the phone is held
    hold() {
        this._want(NORMAL);
    }

    // sideways, whichever way the phone is held: the way it is turned if it is (Android's sensorLandscape, read as
    // it starts), else Android's landscape
    holdSideways() {
        if (this._wanted === LANDSCAPE || this._wanted === SEASCAPE)
            return;
        const turnedRight = !this._held && this._sensor.get_orientation() === Meta.Orientation.LEFT_UP;
        this._want(turnedRight ? SEASCAPE : LANDSCAPE);
    }

    // following the phone
    release() {
        this._want(FOLLOW);
    }

    _want(wanted) {
        this._wanted = wanted;
        if (wanted !== FOLLOW)
            this._grab();
        this._converge();
    }

    // one turn at a time, towards whatever is wanted when it ends
    async _converge() {
        if (this._busy) {
            this._again = true;
            return;
        }
        this._busy = true;
        try {
            do {
                this._again = false;
                const wanted = this._wanted;
                if (wanted === FOLLOW) {
                    // held sideways for an app: upright first, as the phone lying flat has no way of its own
                    if (this._forced && this._held)
                        await this._turnTo(NORMAL);
                    if (this._again)
                        continue;
                    this._forced = false;
                    this._letGo();
                } else {
                    this._forced = wanted !== NORMAL;
                    // always: a panel upside down (180) is portrait-sized, so `upright` would call it done;
                    // _turnTo compares the real transform
                    await this._turnTo(wanted);
                }
            } while (this._again && !this._destroyed);
        } catch (e) {
            logError(e, 'gnome-phone: turning the panel');
        } finally {
            this._busy = false;
        }
    }

    _grab() {
        this._cancelResync();
        if (!this._held) {
            this._held = true;
            this._inhibit(true);
        }
    }

    _letGo() {
        if (!this._held)
            return;
        this._held = false;
        this._inhibit(false);
        this._cancelResync();
        this._resyncId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RESYNC_MS, () => {
            this._resyncId = 0;
            const o = this._sensor.get_orientation();
            if (!this._touchscreen.get_boolean(LOCK_KEY) && this._sensor.has_accelerometer &&
                o !== Meta.Orientation.NORMAL && o !== Meta.Orientation.UNDEFINED)
                this._sensor.emit('orientation-changed');
            return GLib.SOURCE_REMOVE;
        });
    }

    // call back once the panel is upright (at once if it is): the id to cancel it with, 0 if called already
    whenUpright(callback) {
        if (this.upright) {
            callback();
            return 0;
        }
        const id = ++this._nextWait;
        const timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, UPRIGHT_TIMEOUT_MS, () => {
            this._waits.delete(id);
            callback();
            return GLib.SOURCE_REMOVE;
        });
        this._waits.set(id, {timeoutId, callback});
        return id;
    }

    cancelWait(id) {
        const wait = this._waits.get(id);
        if (!wait)
            return;
        GLib.source_remove(wait.timeoutId);
        this._waits.delete(id);
    }

    _monitorsChanged() {
        if (!this.upright)
            return;
        const waits = [...this._waits.values()];
        this._waits.clear();
        for (const {timeoutId, callback} of waits) {
            GLib.source_remove(timeoutId);
            callback();
        }
    }

    async _turnTo(transform) {
        const [serial, monitors, logical] = (await displayConfig('GetCurrentState', null, STATE_TYPE))
            .recursiveUnpack();
        if (logical.length !== 1 || logical[0][3] === transform || !this._held)
            return;
        const [, , scale, , , specs] = logical[0];
        const connector = specs[0][0];
        const mode = monitors.find(([spec]) => spec[0] === connector)?.[1].find(m => m[6]['is-current']);
        if (!mode)
            return;
        await displayConfig('ApplyMonitorsConfig', new GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})',
            [serial, TEMPORARY, [[0, 0, scale, transform, true, [[connector, mode[0], {}]]]], {}]));
    }

    // mutter's tracking, held or let go: with its own call where mutter exports it (gnome-phone's mutter patch 0004),
    // else with GNOME's orientation lock
    _inhibit(held) {
        if (!this._canInhibit) {
            if (!this._warned)
                log('gnome-phone: mutter does not export its orientation tracking inhibit: the panel is held upright ' +
                    'with the orientation lock');
            this._warned = true;
            if (held)
                this._holdLock();
            else
                this._releaseLock();
        } else if (held) {
            this._sensor.inhibit_tracking();
        } else {
            this._sensor.uninhibit_tracking();
        }
    }

    _holdLock() {
        if (lockHolders.has(this))
            return;
        if (!lockHolders.size) {
            userLock = this._touchscreen.get_boolean(LOCK_KEY);
            writeSavedLock(userLock);
        }
        lockHolders.add(this);
        this._touchscreen.set_boolean(LOCK_KEY, true);
    }

    _releaseLock() {
        if (!lockHolders.delete(this) || lockHolders.size)
            return;
        this._touchscreen.set_boolean(LOCK_KEY, userLock);
        dropSavedLock();
    }

    _cancelResync() {
        if (this._resyncId)
            GLib.source_remove(this._resyncId);
        this._resyncId = 0;
    }

    destroy() {
        this._destroyed = true;
        for (const id of [...this._waits.keys()])
            this.cancelWait(id);
        this._cancelResync();
        if (this._syncId)
            GLib.source_remove(this._syncId);
        this._syncId = 0;
        this._sensor.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        this._keyguard?.disconnectObject(this);
        global.display.disconnectObject(this);
        global.window_manager.disconnectObject(this);
        if (this._held)
            this._inhibit(false);
        this._held = false;
    }
}
