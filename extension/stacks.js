// Stacks, the shell's side: which owner (the user, Minerva, an agent) each task window belongs to, the D-Bus API
// org.minerva.Stacks1 through which agents open and close windows in their own stack, and what recents asks (the
// owners that have windows, whose is this window). The rules are stacks-model.js's, which node tests.
//
// An agent's window never takes the screen: it is minimized when it has drawn, as if the user had sent it to recents,
// and the focus goes back to what had it. The user finds it in recents, in the agent's tab, and a touch on the card
// makes it the foreground window like any other.
//
// The tokens are in ~/.config/minerva/stacks.json (stacks-model.js parseConfig); without the file the API refuses
// everything and every window is the user's.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {parseConfig, StackError, StackModel, USER} from './stacks-model.js';

export {USER};

const NAME = 'org.minerva.Stacks1';
const PATH = '/org/minerva/Stacks1';
const ERROR = 'org.minerva.Stacks1.Error';

const IFACE = `<node>
  <interface name="${NAME}">
    <!-- the stacks the token may see, as JSON {you, stacks: [{owner, windows: [{id, app_id, title, pid, focused,
         in_recents}]}]}; windows are the task windows (dialogs are not listed) -->
    <method name="List">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- start an app in the caller's stack; uris may be empty. The window joins the stack when it appears (within
         10 s) and stays out of the user's way: List shows it -->
    <method name="Open">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="as" name="uris" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- close a window of a stack the token may act on: asks the app, kills it if it does not go -->
    <method name="Close">
      <arg type="s" name="token" direction="in"/>
      <arg type="u" name="id" direction="in"/>
      <arg type="b" name="closed" direction="out"/>
    </method>
    <!-- bring a window to the front: a token with the focus grant only -->
    <method name="Focus">
      <arg type="s" name="token" direction="in"/>
      <arg type="u" name="id" direction="in"/>
      <arg type="b" name="focused" direction="out"/>
    </method>
    <!-- a window came or went, or the tokens changed: List again -->
    <signal name="Changed"/>
  </interface>
</node>`;

const configPath = () => GLib.build_filenamev([GLib.get_user_config_dir(), 'minerva', 'stacks.json']);
const desktopId = id => (id.endsWith('.desktop') ? id : `${id}.desktop`);

export class Stacks {
    // home: the launcher's (its window is no task); closeApp: recents.js's, which asks and then kills
    constructor(home, closeApp) {
        this._home = home;
        this._closeApp = closeApp;
        this._model = new StackModel(new Map());
        this._windows = new Map();              // window id -> Meta.Window
        this._tracker = Shell.WindowTracker.get_default();
        this._prevFocus = null;
        this._idles = new Set();                // GLib idle sources of _tuckAway

        this._loadConfig();
        this._monitor = Gio.File.new_for_path(configPath()).monitor_file(Gio.FileMonitorFlags.NONE, null);
        this._monitor.connectObject('changed', () => this._loadConfig(), this);

        global.display.connectObject('window-created', (_d, win) => this._created(win), this);
        for (const actor of global.get_window_actors())
            this._add(actor.meta_window, true);

        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, PATH);
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, NAME, Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    // ---- what recents asks ----------------------------------------------------------------------------------------

    // the owners that have a task window, the user's first
    owners() {
        return this._model.owners();
    }

    ownerOf(win) {
        return this._model.ownerOf(win.get_id());
    }

    // how many task windows the owner has
    count(owner) {
        return this._model.windows(owner).length;
    }

    // ---- tracking -------------------------------------------------------------------------------------------------

    _loadConfig() {
        let tokens = new Map();
        try {
            const [ok, bytes] = Gio.File.new_for_path(configPath()).load_contents(null);
            if (ok)
                tokens = parseConfig(new TextDecoder().decode(bytes));
        } catch (e) {
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
                console.warn(`gnome-phone: stacks config ignored: ${e.message}`);
        }
        this._model.setTokens(tokens);
        this._changed();
    }

    _isTask(win) {
        return win.get_window_type() === Meta.WindowType.NORMAL && !win.skip_taskbar && !win.get_transient_for() &&
            !this._home.isHome(win);
    }

    // The desktop id of the window's app. The shell's tracker names a window it has not matched yet "window:N", so the
    // window's own application id comes first; '' when neither is known (yet).
    _appId(win) {
        const gtk = win.get_gtk_application_id();
        if (gtk)
            return desktopId(gtk);
        const id = this._tracker.get_window_app(win)?.get_id() ?? '';
        return id.startsWith('window:') ? '' : id;
    }

    _created(win) {
        if (!this._isTask(win))
            return;
        // the app is known when the window has said its id; until then wait for its first picture
        if (this._appId(win)) {
            this._add(win, false);
            return;
        }
        const actor = win.get_compositor_private();
        actor?.connectObject('first-frame', () => {
            actor.disconnectObject(this);
            this._add(win, true);
        }, this);
    }

    // drawn: the window has had its first frame already
    _add(win, drawn) {
        if (!this._isTask(win) || this._windows.has(win.get_id()))
            return;
        const id = win.get_id();
        const owner = this._model.assign(id, this._appId(win));
        this._windows.set(id, win);
        win.connectObject('unmanaged', () => this._gone(win), this);
        if (owner !== USER)
            this._tuckAway(win, drawn);
        this._changed();
    }

    _gone(win) {
        win.disconnectObject(this);
        this._windows.delete(win.get_id());
        this._model.release(win.get_id());
        this._changed();
    }

    // an agent's window is for recents, not the screen
    _tuckAway(win, drawn) {
        const actor = win.get_compositor_private();
        if (!actor)
            return;
        const prev = this._prevFocus;
        const done = () => {
            if (!this._windows.has(win.get_id()))
                return;
            Main.wm.skipNextEffect(actor);
            win.minimize();
            actor.opacity = 255;
            const focus = global.display.focus_window;
            if (!focus || focus === win || focus.minimized) {
                const back = prev && !prev.minimized && this._windows.has(prev.get_id()) ? prev : this._home.window;
                back?.activate(global.get_current_time());
            }
        };
        // never inside the frame signal: minimizing there restacks the windows while mutter is painting and its
        // compositor aborts (invalidate_top_window_actor_for_views: frame_in_progress), taking the session with it
        const later = () => {
            const id = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
                this._idles.delete(id);
                done();
                return GLib.SOURCE_REMOVE;
            });
            this._idles.add(id);
        };
        if (drawn) {
            later();
            return;
        }
        Main.wm.skipNextEffect(actor);
        actor.opacity = 0;
        actor.connectObject('first-frame', () => {
            actor.disconnectObject(this);
            later();
        }, this);
    }

    _changed() {
        this._impl?.emit_signal('Changed', null);
    }

    // ---- the API --------------------------------------------------------------------------------------------------

    _describe(win) {
        return {
            id: win.get_id(),
            app_id: this._appId(win),
            title: win.get_title() ?? '',
            pid: win.get_pid(),
            focused: win.has_focus(),
            in_recents: win.minimized,
        };
    }

    _list(token) {
        const caller = this._model.caller(token);
        const stacks = this._model.visible(token).map(owner => ({
            owner,
            windows: this._model.windows(owner).map(id => this._windows.get(id)).filter(Boolean)
                .map(win => this._describe(win)),
        }));
        return {you: caller.name, stacks};
    }

    _open(token, appId, uris) {
        this._model.caller(token);
        const app = Shell.AppSystem.get_default().lookup_app(desktopId(appId));
        if (!app)
            throw new StackError(`no app ${appId}`);
        const claim = this._model.claim(token, app.get_id());
        this._prevFocus = global.display.focus_window;
        try {
            app.get_app_info().launch_uris(uris, null);
        } catch (e) {
            throw new StackError(`cannot start ${appId}: ${e.message}`);
        }
        return {owner: claim.owner, app_id: app.get_id(), note: `the window joins stack ${claim.owner} when it appears`};
    }

    _call(invocation, fn) {
        try {
            const result = fn();
            invocation.return_value(new GLib.Variant('(' + (typeof result === 'boolean' ? 'b' : 's') + ')',
                [typeof result === 'boolean' ? result : JSON.stringify(result)]));
        } catch (e) {
            if (!(e instanceof StackError))
                logError(e, 'gnome-phone: stacks');
            invocation.return_dbus_error(ERROR, e instanceof StackError ? e.message : 'internal error');
        }
    }

    ListAsync([token], invocation) {
        this._call(invocation, () => this._list(token));
    }

    OpenAsync([token, appId, uris], invocation) {
        this._call(invocation, () => this._open(token, appId, uris));
    }

    CloseAsync([token, id], invocation) {
        this._call(invocation, () => {
            this._model.authorizeAct(token, id);
            const win = this._windows.get(id);
            if (!win)
                throw new StackError(`no window ${id}`);
            this._closeApp(win);
            return true;
        });
    }

    FocusAsync([token, id], invocation) {
        this._call(invocation, () => {
            this._model.authorizeFocus(token, id);
            const win = this._windows.get(id);
            if (!win)
                throw new StackError(`no window ${id}`);
            win.activate(global.get_current_time());
            return true;
        });
    }

    destroy() {
        this._monitor?.cancel();
        this._monitor?.disconnectObject(this);
        for (const id of this._idles)
            GLib.source_remove(id);
        this._idles.clear();
        global.display.disconnectObject(this);
        for (const win of this._windows.values()) {
            win.disconnectObject(this);
            win.get_compositor_private()?.disconnectObject(this);
        }
        this._windows.clear();
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
        this._impl = null;
    }
}
