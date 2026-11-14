// Stacks, the shell's side: which owner (the user, Minerva, an agent) each task window belongs to, the D-Bus API
// org.minerva.Stacks1 through which agents open and close windows in their own stack, and what recents asks (the
// owners that have windows, whose is this window). The rules are stacks-model.js's, which node tests.
//
// An agent's window never takes the screen: it is minimized when it has drawn, as if the user had sent it to recents,
// and the focus goes back to what had it. The user finds it in recents, in the agent's tab, and a touch on the card
// makes it the foreground window like any other.
//
// Four things around that, each small:
//  - a memory cap per stack: Open refuses over it, and every WATCH_S seconds, while an agent has windows, the stack
//    is measured (appmem.py, in a process of its own) and its newest window closed while it is over. The kernel
//    cannot cap a stack: D-Bus activated apps all live in dbus.service's cgroup;
//  - the lock: recents shows no agent tabs and an agent sees only its own stack while the phone is locked, and an
//    agent's window does not go over the lock screen without its grant (whenlocked.js asks mayShowWhenLocked);
//  - the take-over: with an agent's window in front, a chip says whose it is; the first touch in the window takes it
//    over (the agent's Close and Focus on it are refused), a tap on the chip hands it back, and it goes back by
//    itself HANDBACK_MS after the user leaves it;
//  - the graph: "opened from" and "switched" edges between windows (stacks-model.js), over Graph.
//
// The tokens are in ~/.config/minerva/stacks.json (stacks-model.js parseConfig); without the file the API refuses
// everything and every window is the user's.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {StackChip} from './stackchip.js';
import {parseConfig, StackError, StackModel, USER} from './stacks-model.js';
import {px} from './units.js';

export {USER};

const NAME = 'org.minerva.Stacks1';
const PATH = '/org/minerva/Stacks1';
const ERROR = 'org.minerva.Stacks1.Error';

const WATCH_S = 30;
const HANDBACK_MS = 120000;
// touches in the status bar's and the gesture bar's bands are the shell's (the shade, home), not the window's
const TOP_BAND = 116, BOTTOM_BAND = 90;

const IFACE = `<node>
  <interface name="${NAME}">
    <!-- the stacks the token may see, as JSON {you, stacks: [{owner, cap_mb, mem_mb, windows: [{id, app_id, title,
         pid, focused, in_recents, taken_over, mem_mb}]}]}; windows are the task windows (dialogs are not listed).
         mem_mb is the last measurement, null before the first. -->
    <method name="List">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- start an app in the caller's stack; uris may be empty. The window joins the stack when it appears (within
         10 s) and stays out of the user's way: List shows it. Refused over the stack's window or memory cap. -->
    <method name="Open">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="as" name="uris" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- Open, from a window the token may see (0: none): the graph gets an "opened_from" edge -->
    <method name="OpenFrom">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="app_id" direction="in"/>
      <arg type="as" name="uris" direction="in"/>
      <arg type="u" name="from" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- close a window of a stack the token may act on: asks the app, kills it if it does not go. Refused on a
         window the user took over. -->
    <method name="Close">
      <arg type="s" name="token" direction="in"/>
      <arg type="u" name="id" direction="in"/>
      <arg type="b" name="closed" direction="out"/>
    </method>
    <!-- bring a window to the front: a token with the focus grant only; never while locked or taken over -->
    <method name="Focus">
      <arg type="s" name="token" direction="in"/>
      <arg type="u" name="id" direction="in"/>
      <arg type="b" name="focused" direction="out"/>
    </method>
    <!-- the windows the token may see and the edges between them, as JSON {you, nodes: [{id, owner, app, title,
         taken_over}], edges: [{kind: "opened_from" | "switched", from, to, n}]}. Kept in memory only. -->
    <method name="Graph">
      <arg type="s" name="token" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <!-- a window came or went, the lock or a take-over changed, or the tokens changed: List again -->
    <signal name="Changed"/>
  </interface>
</node>`;

const configPath = () => GLib.build_filenamev([GLib.get_user_config_dir(), 'minerva', 'stacks.json']);
const desktopId = id => (id.endsWith('.desktop') ? id : `${id}.desktop`);

export class Stacks {
    // home: the launcher's (its window is no task); closeApp: recents.js's, which asks and then kills;
    // measureApps(keys, done, failed): recents.js's, appmem.py in a process of its own, done gets {pid: bytes}
    constructor(home, closeApp, measureApps) {
        this._home = home;
        this._closeApp = closeApp;
        this._measureApps = measureApps;
        this._model = new StackModel(new Map());
        this._windows = new Map();              // window id -> Meta.Window
        this._tracker = Shell.WindowTracker.get_default();
        this._prevFocus = null;
        this._idles = new Set();                // GLib idle sources of _tuckAway
        this._front = null;                     // the task window in front (null: the launcher, or no task)
        this._behind = null;                    // the task window that was in front before it
        this._mem = new Map();                  // window id -> bytes, the last measurement
        this._watchId = 0;
        this._handback = new Map();             // window id -> GLib source: the take-over ends
        this._overview = false;
        this._keyguard = null;
        this._touchWatch = {};                  // the owner of the stage's captured-event handler
        this._chip = new StackChip(() => this._toggleTakeOver());

        this._loadConfig();
        this._monitor = Gio.File.new_for_path(configPath()).monitor_file(Gio.FileMonitorFlags.NONE, null);
        this._monitor.connectObject('changed', () => this._loadConfig(), this);

        global.display.connectObject(
            'window-created', (_d, win) => this._created(win),
            'notify::focus-window', () => this._focusChanged(), this);
        for (const actor of global.get_window_actors())
            this._add(actor.meta_window, true);
        this._focusChanged();

        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, PATH);
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, NAME, Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    // ---- what the shell asks --------------------------------------------------------------------------------------

    // the owners that have a task window, the user's first; locked, the user's alone
    owners() {
        return this._model.uiOwners();
    }

    ownerOf(win) {
        return this._model.ownerOf(win.get_id());
    }

    // how many task windows the owner has
    count(owner) {
        return this._model.windows(owner).length;
    }

    // keyguard.js's Keyguard: showing, 'changed'
    setKeyguard(keyguard) {
        this._keyguard = keyguard;
        keyguard.connectObject('changed', () => this._lockChanged(), this);
        this._lockChanged();
    }

    // whenlocked.js: whether the window may be the one over the lock screen
    mayShowWhenLocked(win) {
        return this._model.mayShowWhenLocked(win.get_id());
    }

    // recents.js: the overview is up (the chip steps aside) or gone
    setOverview(on) {
        this._overview = on;
        this._syncChip();
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

    // the window the new one came from: the one in front, or, when the new one already is, the one before it
    _parentFor(win) {
        const parent = this._front && this._front !== win ? this._front : this._behind;
        return parent && parent !== win && this._windows.has(parent.get_id()) ? parent.get_id() : 0;
    }

    // drawn: the window has had its first frame already
    _add(win, drawn) {
        if (!this._isTask(win) || this._windows.has(win.get_id()))
            return;
        const id = win.get_id();
        const owner = this._model.assign(id, this._appId(win), this._parentFor(win));
        this._windows.set(id, win);
        win.connectObject('unmanaged', () => this._gone(win), this);
        if (owner !== USER) {
            this._tuckAway(win, drawn);
            this._ensureWatch();
        }
        this._changed();
    }

    _gone(win) {
        const id = win.get_id();
        win.disconnectObject(this);
        this._windows.delete(id);
        this._mem.delete(id);
        this._model.release(id);
        this._dropHandback(id);
        if (this._front === win)
            this._front = null;
        if (this._behind === win)
            this._behind = null;
        this._syncChip();
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

    // ---- the lock -------------------------------------------------------------------------------------------------

    _lockChanged() {
        const locked = !!this._keyguard?.showing;
        if (locked === this._model.locked)
            return;
        this._model.locked = locked;
        this._syncChip();
        this._changed();
    }

    // ---- the focus, the graph and the take-over -------------------------------------------------------------------

    _focusChanged() {
        const focus = global.display.focus_window;
        const task = focus && this._isTask(focus) ? focus : null;
        const prev = this._front;
        if (task === prev)
            return;
        if (task) {
            if (prev)
                this._behind = prev;
            this._front = task;
            if (prev && this._windows.has(prev.get_id()) && this._windows.has(task.get_id()))
                this._model.switched(prev.get_id(), task.get_id());
        } else {
            // the launcher, or something that is no task: what is launched next did not come from a window
            this._front = null;
            this._behind = null;
        }
        // the user left a window they had taken over: it goes back to its agent after a while
        if (prev && this._model.isTakenOver(prev.get_id()))
            this._startHandback(prev.get_id());
        if (task)
            this._dropHandback(task.get_id());
        this._syncChip();
    }

    _startHandback(id) {
        this._dropHandback(id);
        this._handback.set(id, GLib.timeout_add(GLib.PRIORITY_DEFAULT, HANDBACK_MS, () => {
            this._handback.delete(id);
            if (this._model.resume(id))
                this._changed();
            return GLib.SOURCE_REMOVE;
        }));
    }

    _dropHandback(id) {
        const source = this._handback.get(id);
        if (source)
            GLib.source_remove(source);
        this._handback.delete(id);
    }

    // The chip is up for an agent's window in front, outside the overview and the lock screen. While it is up and the
    // window is not taken over, the first touch in the window takes it over.
    _syncChip() {
        const win = this._front;
        const id = win?.get_id();
        const show = !!win && this._windows.has(id) && this._model.ownerOf(id) !== USER && !win.minimized &&
            !this._overview && !this._model.locked;
        global.stage.disconnectObject(this._touchWatch);
        if (!show) {
            this._chip.hide();
            return;
        }
        const taken = this._model.isTakenOver(id);
        this._chip.show(this._model.ownerOf(id), taken);
        if (!taken) {
            global.stage.connectObject('captured-event', (_stage, event) => this._touched(event), this._touchWatch);
        }
    }

    _touched(event) {
        const type = event.type();
        if (type !== Clutter.EventType.TOUCH_BEGIN && type !== Clutter.EventType.BUTTON_PRESS)
            return Clutter.EVENT_PROPAGATE;
        const [x, y] = event.get_coords();
        const m = Main.layoutManager.primaryMonitor;
        const r = this._chip.rect;
        if (y < m.y + px(TOP_BAND) || y > m.y + m.height - px(BOTTOM_BAND) ||
            (r && x >= r[0] && x < r[2] && y >= r[1] && y < r[3]))
            return Clutter.EVENT_PROPAGATE;
        const id = this._front?.get_id();
        if (id && this._model.takeOver(id)) {
            this._syncChip();
            this._changed();
        }
        return Clutter.EVENT_PROPAGATE;
    }

    _toggleTakeOver() {
        const id = this._front?.get_id();
        if (!id)
            return;
        if (this._model.isTakenOver(id))
            this._model.resume(id);
        else
            this._model.takeOver(id);
        this._syncChip();
        this._changed();
    }

    // ---- the memory cap -------------------------------------------------------------------------------------------

    // what the owner's stack uses now, in MB (measured now, in a process of its own); 0 for a stack with no window
    _usedMb(owner) {
        const keys = this._model.windows(owner).map(id => `${this._windows.get(id)?.get_pid()}`).filter(k => k !== 'undefined');
        if (!keys.length)
            return Promise.resolve(0);
        return new Promise(resolve => {
            this._measureApps(keys, bytes => {
                for (const id of this._model.windows(owner)) {
                    const b = bytes[`${this._windows.get(id)?.get_pid()}`];
                    if (b)
                        this._mem.set(id, b);
                }
                resolve(Object.values(bytes).reduce((a, b) => a + b, 0) / (1024 * 1024));
            }, () => resolve(0));
        });
    }

    // the watch runs only while an agent has a window
    _ensureWatch() {
        if (this._watchId)
            return;
        this._watchId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, WATCH_S, () => {
            if (!this._agentOwners().length) {
                this._watchId = 0;
                return GLib.SOURCE_REMOVE;
            }
            this._watch().catch(e => logError(e, 'gnome-phone: stacks watch'));
            return GLib.SOURCE_CONTINUE;
        });
    }

    _agentOwners() {
        return this._model.owners().filter(o => o !== USER);
    }

    async _watch() {
        for (const owner of this._agentOwners()) {
            const mb = await this._usedMb(owner);
            const id = this._model.overCap(owner, mb);
            const win = id ? this._windows.get(id) : null;
            if (win) {
                console.warn(`gnome-phone: stack ${owner} uses ${Math.round(mb)} MB of ${this._model.capMb(owner)} MB: ` +
                    `closing its newest window (${this._appId(win)})`);
                this._closeApp(win);
            }
        }
        this._changed();
    }

    // ---- the API --------------------------------------------------------------------------------------------------

    _describe(win) {
        const id = win.get_id();
        const bytes = this._mem.get(id);
        return {
            id,
            app_id: this._appId(win),
            title: win.get_title() ?? '',
            pid: win.get_pid(),
            focused: win.has_focus(),
            in_recents: win.minimized,
            taken_over: this._model.isTakenOver(id),
            mem_mb: bytes ? Math.round(bytes / (1024 * 1024)) : null,
        };
    }

    _list(token) {
        const caller = this._model.caller(token);
        const stacks = this._model.visible(token).map(owner => {
            const ids = this._model.windows(owner).filter(id => this._windows.has(id));
            const mem = ids.reduce((a, id) => a + (this._mem.get(id) ?? 0), 0);
            return {
                owner,
                cap_mb: this._model.capMb(owner) || null,
                mem_mb: mem ? Math.round(mem / (1024 * 1024)) : null,
                windows: ids.map(id => this._describe(this._windows.get(id))),
            };
        });
        return {you: caller.name, stacks};
    }

    _graph(token) {
        const graph = this._model.graph(token);
        for (const node of graph.nodes)
            node.title = this._windows.get(node.id)?.get_title() ?? '';
        return graph;
    }

    async _open(token, appId, uris, from) {
        const caller = this._model.caller(token);
        const app = Shell.AppSystem.get_default().lookup_app(desktopId(appId));
        if (!app)
            throw new StackError(`no app ${appId}`);
        if (caller.admin)
            this._model.claim(token, app.get_id());        // throws: an admin owns no windows
        const usedMb = await this._usedMb(caller.name);
        const claim = this._model.claim(token, app.get_id(), {from, usedMb});
        this._prevFocus = global.display.focus_window;
        try {
            app.get_app_info().launch_uris(uris, null);
        } catch (e) {
            throw new StackError(`cannot start ${appId}: ${e.message}`);
        }
        return {owner: claim.owner, app_id: app.get_id(), note: `the window joins stack ${claim.owner} when it appears`};
    }

    _call(invocation, fn) {
        let done;
        try {
            done = Promise.resolve(fn());
        } catch (e) {
            done = Promise.reject(e);
        }
        done.then(result => {
            invocation.return_value(new GLib.Variant('(' + (typeof result === 'boolean' ? 'b' : 's') + ')',
                [typeof result === 'boolean' ? result : JSON.stringify(result)]));
        }).catch(e => {
            if (!(e instanceof StackError))
                logError(e, 'gnome-phone: stacks');
            invocation.return_dbus_error(ERROR, e instanceof StackError ? e.message : 'internal error');
        });
    }

    ListAsync([token], invocation) {
        this._call(invocation, () => this._list(token));
    }

    OpenAsync([token, appId, uris], invocation) {
        this._call(invocation, () => this._open(token, appId, uris, 0));
    }

    OpenFromAsync([token, appId, uris, from], invocation) {
        this._call(invocation, () => this._open(token, appId, uris, from));
    }

    GraphAsync([token], invocation) {
        this._call(invocation, () => this._graph(token));
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
        if (this._watchId)
            GLib.source_remove(this._watchId);
        this._watchId = 0;
        for (const source of this._handback.values())
            GLib.source_remove(source);
        this._handback.clear();
        global.stage.disconnectObject(this._touchWatch);
        this._keyguard?.disconnectObject(this);
        this._keyguard = null;
        this._chip.destroy();
        this._chip = null;
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
