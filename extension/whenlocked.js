// Windows over the lock screen: Android's showWhenLocked. An app whose desktop file says
// X-GnomePhone-ShowWhenLocked=true (Android declares android:showWhenLocked in the manifest) may show a window over
// the lock screen: the ringing alarm, an incoming call. While such a window is the top one, the keyguard is occluded
// (KeyguardViewMediator.setOccluded): its view and its grab step aside for that window, and only for it. The phone stays
// locked meanwhile: the other windows stay hidden, and home, recents and the shade stay shut (navigation.js, shade.js
// ask keyguard.showing). When the window closes, or goes behind another, the lock screen is back.
//
// An app gets on the lock screen this way only by putting its window up while the phone is locked (an alarm that
// rings, a call coming in), or by having it on top when the phone locks (a call under way), as on Android.
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

const KEY = 'X-GnomePhone-ShowWhenLocked';

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
        keyguard.connectObject('changed', () => this._sync(), this);
        global.display.connectObject(
            'window-created', (_display, win) => this._watch(win),
            'restacked', () => this._sync(), this);
        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);
        this._sync();
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
        this._keyguard.setOccluded(top && showsWhenLocked(top) && this._stacks.mayShowWhenLocked(top) ? top : null);
    }

    destroy() {
        this._keyguard.disconnectObject(this);
        global.display.disconnectObject(this);
        for (const actor of global.get_window_actors())
            actor.meta_window.disconnectObject(this);
        this._keyguard.setOccluded(null);
    }
}
