// The home screen: the Neo launcher, a Vulkan Wayland client the shell starts itself, so it knows the launcher's
// window from every other. That window is made the desktop (under every other window, on every workspace, in no
// window list) and comes back if the launcher dies.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {FitWindows} from './fit.js';
import {SingleStack} from './stack.js';

// a hand-built launcher in /usr/local comes before the packaged one, as keyguard.js's gp-auth does
const LAUNCHERS = ['/usr/local/bin/neo-launcher', '/usr/bin/neo-launcher'];
const RESPAWN_MS = 1000;

export class Home {
    constructor() {
        this._client = null;
        this._window = null;
        this._respawnId = 0;
        this._destroyed = false;

        global.display.connectObject('window-created', (_d, win) => this._windowCreated(win), this);
        // the launcher taking the focus (home, back to home, recents closed) closes a keyboard an app left open: its
        // text field is minimized, but the input method's focus-out does not always follow
        global.display.connectObject('notify::focus-window', () => {
            if (this.isHome(global.display.focus_window) && Main.keyboard.visible)
                Main.keyboard.close();
        }, this);
        this._spawn();
        this._takeOverview();
        this._stack = new SingleStack(this);
        this._fit = new FitWindows(this);
    }

    // The overview was the mobile shell's home: whatever would show it goes home instead
    _takeOverview() {
        const overview = Main.overview;
        this._overview = {show: overview.show, showApps: overview.showApps, toggle: overview.toggle};
        overview.show = () => this.goHome();
        overview.showApps = () => this.goHome();
        overview.toggle = () => this.goHome();
        const hide = () => {
            if (overview.visible)
                overview.hide();
        };
        if (Main.layoutManager._startingUp)
            Main.layoutManager.connectObject('startup-complete', hide, this);
        else
            hide();
    }

    _giveOverview() {
        Object.assign(Main.overview, this._overview);
        Main.layoutManager.disconnectObject(this);
    }

    // Home: every app window on this workspace goes to recents (minimized), the launcher takes the focus
    goHome() {
        const workspace = global.workspace_manager.get_active_workspace();
        for (const win of workspace.list_windows()) {
            if (win === this._window || win.minimized || win.get_window_type() !== Meta.WindowType.NORMAL)
                continue;
            win.minimize();
        }
        this._window?.activate(global.get_current_time());
    }

    get window() {
        return this._window;
    }

    isHome(win) {
        return win !== null && win === this._window;
    }

    _spawn() {
        // the launcher gets the shell's environment: a GPU whose Vulkan driver needs a variable (panvk's
        // PAN_I_WANT_A_BROKEN_VULKAN_DRIVER on a Mali) has it set for the session, in environment.d, by its device
        // package; the launcher picks a GPU over a software rasterizer itself
        const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
        const exe = LAUNCHERS.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
        if (!exe) {
            console.warn('gnome-phone: no neo-launcher to start');
            return;
        }
        try {
            this._client = Meta.WaylandClient.new_subprocess(global.context, launcher, [exe]);
        } catch (e) {
            logError(e, 'gnome-phone: cannot start the launcher');
            return;
        }
        const proc = this._client.get_subprocess();
        proc.wait_async(null, () => {
            if (this._destroyed)
                return;
            console.warn(`gnome-phone: the launcher exited (${proc.get_exit_status()}), starting it again`);
            this._client = null;
            this._window = null;
            this._respawnId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RESPAWN_MS, () => {
                this._respawnId = 0;
                this._spawn();
                return GLib.SOURCE_REMOVE;
            });
        });
    }

    _windowCreated(win) {
        if (!this._client?.owns_window(win))
            return;
        this._window = win;
        win.set_type(Meta.WindowType.DESKTOP);
        win.stick();
        win.connectObject('unmanaged', () => {
            if (this._window === win)
                this._window = null;
        }, this);
    }

    destroy() {
        this._destroyed = true;
        this._fit.destroy();
        this._stack.destroy();
        this._giveOverview();
        global.display.disconnectObject(this);
        this._window?.disconnectObject(this);
        if (this._respawnId)
            GLib.source_remove(this._respawnId);
        this._client?.get_subprocess()?.force_exit();
        this._client = null;
    }
}
