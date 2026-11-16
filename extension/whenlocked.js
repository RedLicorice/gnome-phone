// Windows over the lock screen: Android's showWhenLocked. An app whose desktop file says
// X-GnomePhone-ShowWhenLocked=true (Android declares android:showWhenLocked in the manifest) may show a window over
// the lock screen: the ringing alarm, an incoming call. While such a window is the top one, the keyguard draws nothing
// and lets the window show through (KeyguardViewMediator.setOccluded), but keeps its place and its modal grab: touches
// and keys stay the lock screen's, so the swipe up brings the bouncer and a finger unlocks, and nothing typed reaches
// the window. The phone stays locked meanwhile: the other windows stay hidden, and home, recents and the shade stay
// shut (navigation.js, shade.js ask keyguard.showing). de.yesman.neo lock-over-window turns the rule off for every app.
// When the window closes, or goes behind another, or the user is unlocking, the lock screen is back.
//
// Only once the secret has been given: a window steps behind the lock screen only while the phone does not want its PIN
// or pattern (keyguard.secretRequired, lockover.js). After a boot or a shell restart, in Lockdown, during the PIN's
// retry timeout, past the sensor's misses, the lock screen stays in front of every window, so the PIN pad is where the
// user will look for it.
//
// An app gets on the lock screen this way only by putting its window up while the phone is locked (an alarm that
// rings, a call coming in), or by having it on top when the phone locks (a call under way), as on Android.
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import {mayOcclude} from './lockover.js';

const KEY = 'X-GnomePhone-ShowWhenLocked';
// the user's switch, de.yesman.neo lock-over-window: off keeps every app window behind the lock screen
const SCHEMA = 'de.yesman.neo';
const OVER_KEY = 'lock-over-window';

// whether @win's app declares that it may show over the lock screen
export function showsWhenLocked(win) {
    const info = Shell.WindowTracker.get_default().get_window_app(win)?.get_app_info();
    return info instanceof Gio.DesktopAppInfo && info.has_key(KEY) && info.get_boolean(KEY);
}

export class WhenLocked {
    // @keyguard: keyguard.js's Keyguard (showing, setOccluded, 'changed'); @stacks: stacks.js, which says whether a
    // window may go over the lock screen (an agent's does not, unless its grant says so)
    constructor(keyguard, stacks) {
        this._keyguard = keyguard;
        this._stacks = stacks;
        const schema = Gio.SettingsSchemaSource.get_default()?.lookup(SCHEMA, true);
        this._settings = schema ? new Gio.Settings({settings_schema: schema}) : null;
        this._settings?.connectObject(`changed::${OVER_KEY}`, () => this._sync(), this);
        keyguard.connectObject('changed', () => this._sync(), this);
        global.display.connectObject(
            'window-created', (_display, win) => this._watch(win),
            'restacked', () => this._sync(), this);
        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);
        this._sync();
    }

    // whether the switch allows any window over the lock screen (a schema too old to know the key reads as on)
    get _allowed() {
        const settings = this._settings;
        return settings?.settings_schema?.has_key(OVER_KEY) ? settings.get_boolean(OVER_KEY) : true;
    }

    _watch(win) {
        // the app is known, and the window has something to show, once it is shown
        win.connectObject(
            'shown', () => this._sync(),
            'notify::minimized', () => this._sync(),
            'unmanaged', () => {
                win.disconnectObject(this);
                this._sync();
            }, this);
    }

    // the top app window on the screen, as the stack has it (the launcher and the shell's own surfaces aside)
    _topWindow() {
        const workspace = global.workspace_manager.get_active_workspace();
        const windows = workspace.list_windows().filter(win =>
            win.get_window_type() === Meta.WindowType.NORMAL && !win.minimized && win.get_compositor_private());
        return global.display.sort_windows_by_stacking(windows).at(-1) ?? null;
    }

    _sync() {
        const top = this._keyguard.showing ? this._topWindow() : null;
        const overWindow = top && showsWhenLocked(top) && this._stacks.mayShowWhenLocked(top) ? top : null;
        this._keyguard.setOccluded(mayOcclude({
            showing: this._keyguard.showing,
            revealing: this._keyguard.revealing,
            allowed: this._allowed,
            requiresSecret: this._keyguard.secretRequired,
            overWindow,
        }) ? overWindow : null);
    }

    destroy() {
        this._keyguard.disconnectObject(this);
        this._settings?.disconnectObject(this);
        global.display.disconnectObject(this);
        for (const actor of global.get_window_actors())
            actor.meta_window.disconnectObject(this);
        this._keyguard.setOccluded(null);
    }
}
