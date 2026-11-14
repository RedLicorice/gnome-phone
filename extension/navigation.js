// Gesture navigation: Android 16's swipe up from the bottom edge (Launcher3 quickstep; spec
// gestures-recents-back.md §1). The app follows the finger, shrinking toward its recents card. Let go and it flies
// home into its icon on RectFSpringAnim's three springs while the launcher scales the home back in (the launcher
// does its half itself, told over de.yesman.NeoLauncher), or it settles back to full screen; hold it first and the
// other tasks spring in beside it, and letting go settles it into recents (recents.js). Along the bar the tasks
// come in at once and page under the finger: quick switch (§3).
//
// On the home screen the same swipe is NoButtonNavbarToOverviewTouchController's: the workspace shrinks toward
// HINT_STATE under the finger; a pause takes it on to OVERVIEW, the cards coming in from the side; let go before one
// and it springs back.
//
// Every length here is in the S10e's device pixels (1080 × 2280, 3 px/dp), the spec's own numbers; actors are
// placed in stage units (÷ the monitor scale).
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {EdgeBack} from './back.js';
import {SidewaysGestures, sideways} from './sideways.js';
import {capturing} from './capture.js';
import {VelocityTracker} from './motion.js';
import {forgetClosing, HINT_SCRIM, PAGER_SNAP_MS, Recents, SETTLE_MS, warmUp} from './recents.js';
import {
    ACCELERATE, DECELERATE, DECELERATE_3, FRAME_MS, FloatingIcon, HOME_ICON_PX, LINEAR, NAV_GESTURE_H,
    OVERSHOOT_1_2, SCALE_REVEAL, SCREEN_H, SCREEN_W, SCROLL, SCROLL_CUBIC, Spring, TASK, TASK_CORNER_R, TASK_PIVOT,
    TASK_W, animate, clamp, dropKeptViews, lerp, lerpRect, warmViews,
} from './task.js';

const TOUCH_SLOP = 34;                  // 8 dp × √2, on the hypotenuse
const QUICKSWITCH_DEG = 15;
const PAUSE_MIN_UP = 108;               // 36 dp of travel before a pause counts
const DRAG_LENGTH = 540;                // H − the recents card's bottom
const MAX_SHIFT = SCREEN_H / DRAG_LENGTH;
const FLING_PX_MS = 1.5;                // 0.5 dp/ms
const MAX_SWIPE_MS = 350;
const DURATION_MULT = 1 / 0.7;
const RESIST_TRANS_END = -551.6;

// MotionPauseDetector (px/ms)
const MP_VERY_SLOW = 0.0855;
const MP_SOMEWHAT_FAST = 0.855;
const MP_FAST = 4.2;
const MP_RAPID_DECEL = 0.6;
const MP_FORCE_PAUSE_MS = 300;

// the flight home (RectFSpringAnim)
const HOME_X = [450, 0.965];
const HOME_Y = [400, 0.95];
const HOME_P = [500, 0.99];
const HOME_VX = [300, 500];             // px/s: the least start velocity, and where the soft cap starts
const HOME_VY = [2000, 5000];
const HOME_V_FALLOFF = 1.4;
const HOME_WIN_ALPHA_END = 0.85;        // window alpha 1 − (p/0.85)³
const HOME_DEFAULT_CENTER = [SCREEN_W / 2, SCREEN_H - 500];    // no icon: availH 2092 − the hotseat's 312
const SHOW_ICON_WAIT_MS = 250;              // a launcher that never draws its icon again: the flying one goes anyway
const DOCK_TOP = SCREEN_H - 312;
const REVEAL_SCALE_FROM = 0.85;
const REVEAL_MS = 1000;
const REVEAL_PIVOT = [SCREEN_W / 2, SCREEN_H / 2];

// from the home screen (NoButtonNavbarToOverviewTouchController, HintState, OverviewState)
const HINT_SCALE = 0.92;                // HintState's workspace
const HINT_DEPTH = 0.05;
const HINT_MS = 80;                     // its transition, the rest of the way after a fling
const HINT_SPRING = [200, 0.7];         // HINT → NORMAL: hint_scale_stiffness, _damping_ratio …
const HINT_SPRING_V = 0.9;              // … and _velocity_dp_per_s, 0.3 dp (as scale per second)
const HOME_PAUSE_MIN_UP = 24;           // the touch slop
const HOME_FLING = 3;                   // px/ms, 1 dp/ms
const OVERVIEW_IN_MS = 380;             // OVERVIEW_SLIDE_IN_DURATION
const OVERVIEW_WS_SCALE = TASK_W / 1032;    // the workspace down to a card's width (the cell layout is 1032)
const OVERVIEW_WS_TY = -(312 - 36) / 2; // up by half the default swipe height (all-apps cell − its label)
const OVERVIEW_DEPTH = 0.7;
const OVERVIEW_MOVEMENT = 0.25;         // the cards follow the finger this much once there

const Target = {HOME: 'home', RECENTS: 'recents', NEW_TASK: 'new-task', LAST_TASK: 'last-task'};

// RectFSpringAnim's start velocity: at least `min`, and past `softMax` only the 1/1.4th power of the rest
function adjustVelocity(v, [min, softMax]) {
    const a = Math.abs(v), sign = Math.sign(v);
    if (a < min)
        return min * sign;
    if (a <= softMax)
        return v;
    return (softMax + Math.pow(a - softMax, 1 / HOME_V_FALLOFF)) * sign;
}

// ---- MotionPauseDetector ------------------------------------------------------------------------------------------

class MotionPause {
    constructor(onFirstPause) {
        this._onFirstPause = onFirstPause;
        this._timeoutId = 0;
        this.reset();
    }

    reset() {
        this.paused = false;
        this.everPaused = false;
        this.disallow = false;
        this._prev = null;
        this._clearTimeout();
    }

    _clearTimeout() {
        if (this._timeoutId)
            GLib.source_remove(this._timeoutId);
        this._timeoutId = 0;
    }

    _set(paused) {
        if (this.disallow)
            paused = false;
        if (paused === this.paused)
            return;
        this.paused = paused;
        if (paused && !this.everPaused) {
            this.everPaused = true;
            this._onFirstPause();
        }
    }

    // vy: the finger's speed along y now, px/ms
    add(vy) {
        // no move for 300 ms is a pause whatever the speeds said
        this._clearTimeout();
        this._timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, MP_FORCE_PAUSE_MS, () => {
            this._timeoutId = 0;
            this._set(true);
            return GLib.SOURCE_REMOVE;
        });
        if (this._prev === null) {
            this._prev = vy;
            return;
        }
        const speed = Math.abs(vy), prev = Math.abs(this._prev);
        let paused;
        if (this.paused) {
            paused = speed < MP_FAST || prev < MP_FAST;
        } else if (Math.sign(vy) !== Math.sign(this._prev)) {
            paused = false;
        } else {
            paused = speed < MP_VERY_SLOW && prev < MP_VERY_SLOW;
            if (!paused && !this.everPaused)
                paused = speed < MP_RAPID_DECEL * prev && speed < MP_SOMEWHAT_FAST;
        }
        this._set(paused);
        this._prev = vy;
    }

    destroy() {
        this._clearTimeout();
    }
}

// ---- the gesture --------------------------------------------------------------------------------------------------

export class Navigation {
    constructor(home, boost) {
        this.home = home;
        this.boost = boost;
        this.icons = new Map();
        this.scale = global.display.get_monitor_scale(Main.layoutManager.primaryIndex);
        this._tracker = new VelocityTracker();
        this._pause = new MotionPause(() => this._paused());
        this.drawerOpen = false;
        this._g = null;
        this._anim = null;
        this._recents = null;

        this._gesture = new Clutter.PanGesture({
            name: 'gnome-phone swipe up',
            begin_threshold: TOUCH_SLOP / this.scale,
            max_n_points: 1,
        });
        this._gesture.connectObject(
            'should-handle-sequence', (_g, event) => this._shouldHandle(event),
            'recognize', () => this._begin(),
            'pan-update', () => this._move(),
            'end', () => this._release(false),
            'cancel', () => this._release(true),
            this);
        global.stage.add_action_full('gnome-phone-swipe-up', Clutter.EventPhase.CAPTURE, this._gesture);

        // the mobile shell's own swipe up (to its overview), and its home bar; stock GNOME Shell has neither
        this._edgeDrag = Main.overview._singleFingerOverviewEdgeDrag;
        if (this._edgeDrag)
            this._edgeDrag.enabled = false;
        this._homeBar = Main.layoutManager.bottomPanelBox ?? null;
        if (this._homeBar) {
            Main.layoutManager.untrackChrome(this._homeBar);
            this._homeBar.hide();
        }

        this._back = new EdgeBack(this);
        // the panel sideways (an app in landscape): sideways.js's swipes, not these
        this._sideways = new SidewaysGestures(this);

        this._warmId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            this._warmId = 0;
            warmUp();
            warmViews();
            return GLib.SOURCE_REMOVE;
        });
    }

    // The launcher's icons: app id → its rect on the current page or the dock (device px; w 0 when it is not
    // there) and its picture
    setIcons(icons) {
        this.icons = new Map(icons.map(([id, rect, path]) => [id, {rect, path}]));
    }

    launcher(method, params) {
        Gio.DBus.session.call('de.yesman.NeoLauncher', '/de/yesman/NeoLauncher', 'de.yesman.NeoLauncher', method,
            params, null, Gio.DBusCallFlags.NO_AUTO_START, 1000, null, null);
    }

    launcherState(state, t0 = 0, app = '') {
        this.launcher('State', new GLib.Variant('(sxs)', [state, t0, app]));
    }

    // SystemUI's status bar: clear over a window being dragged (Android's bar has no background of its own)
    statusBarOverApp(value) {
        Main.layoutManager.uiGroup.get_children().find(a => a.name === 'gnomePhoneStatusBar')?.setOverApp?.(value);
    }

    // the launcher, at rest on its workspace, with nothing over it
    _atHome() {
        const focus = global.display.focus_window;
        return !!this.home.window && !this.drawerOpen && (!focus || this.home.isHome(focus));
    }

    shadeOpen() {
        return !!Main.layoutManager.uiGroup.get_children().find(a => a.name === 'gnomePhoneShade')?.isOpen;
    }

    // a swipe or its animation is under way
    get busy() {
        return !!(this._g || this._anim);
    }

    // the lock screen: while it is up its grab keeps touches from the stage, so back watches it; back from its
    // bouncer goes to the lock screen (StatusBarKeyguardViewManager.onBackPressed)
    setKeyguard(keyguard) {
        this._keyguard = keyguard;
        let locked = keyguard.showing;
        this._keyguardId = keyguard.connect('changed', () => {
            this.watchBack(keyguard.showing ? keyguard.actor : null);
            // locked: the launcher closes its menus and its drawer, as Launcher3 goes to NORMAL when the screen goes
            // off, so nothing of them is waiting after the unlock
            if (keyguard.showing && !locked)
                this.launcher('Locked', null);
            locked = keyguard.showing;
        });
    }

    get bouncerShown() {
        return !!this._keyguard?.bouncerShowing;
    }

    keyguardBack() {
        this._keyguard?.back();
    }

    get recentsShown() {
        return !!this._recents?.shown;
    }

    recentsBack() {
        this._recents?.back();
    }

    // KEYCODE_APP_SWITCH (OverviewCommandHelper's TYPE_TOGGLE), from the Bixby key: an app settles into its card as
    // a paused swipe lets it, the home screen goes HINT → OVERVIEW as after a pause, and in recents it's back again
    toggleRecents() {
        if (this._recents?.shown) {
            this._recents.back();
            return;
        }
        if (this._g || this._anim || this._recents || this._back.busy || Main.actionMode !== Shell.ActionMode.NORMAL ||
            this.shadeOpen() || this._keyguard?.showing)
            return;
        const win = this._appWindow();
        if (win) {
            this.onGestureBegin?.();
            this.launcherState('background');
            this.statusBarOverApp(false);
            const recents = this._recents = new Recents(this, win);
            this._g = {win, recents, shift: 0, rect: [0, 0, SCREEN_W, SCREEN_H], r: 0, x: 0};
            recents.attach();
            this._settle(1, null, OVERSHOOT_1_2, () => {
                this._g = null;
                recents.show();
            }, SETTLE_MS, t => recents.setScrim(LINEAR(t)));
        } else if (this._atHome()) {
            this.onGestureBegin?.();
            const recents = this._recents = new Recents(this, null);
            // let go already: the overview is up as soon as the cards are in
            this._g = {home: true, recents, p: 0, x: SCREEN_W / 2, y: SCREEN_H, entering: false, reached: false,
                released: true, anim: null};
            this._homeToOverview();
        }
    }

    // the actor whose grab keeps touches from the stage (recents), or null: back watches its touches
    watchBack(actor) {
        this._back.watch(actor);
    }

    // HapticFeedbackConstants.GESTURE_THRESHOLD_(DE)ACTIVATE: feedbackd's shortest tick
    haptic() {
        Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback',
            'TriggerFeedback', new GLib.Variant('(ssa{sv}i)', ['de.yesman.GnomePhone', 'button-released', {}, -1]),
            null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
    }

    // a key press for the focused window, with modifiers held
    key(keyval, modifiers = []) {
        this._keyboard ??= global.stage.context.get_backend().get_default_seat()
            .create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        let t = GLib.get_monotonic_time();
        for (const m of modifiers)
            this._keyboard.notify_keyval(t++, m, Clutter.KeyState.PRESSED);
        this._keyboard.notify_keyval(t++, keyval, Clutter.KeyState.PRESSED);
        this._keyboard.notify_keyval(t++, keyval, Clutter.KeyState.RELEASED);
        for (const m of [...modifiers].reverse())
            this._keyboard.notify_keyval(t++, m, Clutter.KeyState.RELEASED);
    }

    // a window recents shows as a task
    isTask(win) {
        return !this.home.isHome(win) && win.get_window_type() === Meta.WindowType.NORMAL && !win.skip_taskbar &&
            !win.get_transient_for();
    }

    // The app on screen: the focused task; with the launcher focused (or nothing), the topmost task stacked over it.
    // A window shorter than the app area leaves a strip of the launcher bare at the bottom (Waydroid's display was
    // 753 px under a 761 px app area on the POCO), and a touch there focuses the launcher before the swipe up sees
    // it: the swipe then took the at-home path and the app sprang back.
    _appWindow() {
        const focus = global.display.focus_window;
        if (focus && !this.home.isHome(focus))
            return !focus.minimized && this.isTask(focus) ? focus : null;
        const ws = global.workspace_manager.get_active_workspace();
        const stack = global.display.sort_windows_by_stacking(ws.list_windows());
        for (let i = stack.length - 1; i >= 0; i--) {
            const w = stack[i];
            if (w.minimized || w.is_hidden())
                continue;
            if (this.home.isHome(w))
                return null;
            if (this.isTask(w))
                return w;
        }
        return null;
    }

    recentsClosed(recents) {
        if (this._recents === recents)
            this._recents = null;
    }

    _shouldHandle(event) {
        if (event.type() !== Clutter.EventType.TOUCH_BEGIN || this._g || this._anim || this._recents)
            return false;
        if (sideways() || this._sideways?.busy)
            return false;
        if (this._back.busy)
            return false;
        if (Main.actionMode !== Shell.ActionMode.NORMAL || this.shadeOpen() || (!this._appWindow() && !this._atHome()))
            return false;
        // locked with an app's window over the lock screen (whenlocked.js): no way home past the lock
        if (this._keyguard?.showing)
            return false;
        const [x, y] = event.get_coords();
        if (y * this.scale < SCREEN_H - NAV_GESTURE_H)
            return false;
        this._down = {x: x * this.scale, y: y * this.scale};
        this._tracker.clear();
        this._tracker.add(event.get_time(), this._down.x, this._down.y);
        this._lastT = event.get_time();
        this._pause.reset();
        return true;
    }

    _begin() {
        this.onGestureBegin?.();
        const win = this._appWindow();
        if (!win) {
            if (this._atHome())
                this._beginHome();
            else
                this._gesture.cancel();
            return;
        }
        this.launcherState('background');
        this.statusBarOverApp(false);
        const recents = this._recents = new Recents(this, win);
        this._g = {win, recents, shift: 0, rect: [0, 0, SCREEN_W, SCREEN_H], r: 0, x: this._down.x};
        this._move();
    }

    _move() {
        const g = this._g;
        if (!g)
            return;
        const c = this._gesture.get_centroid_abs();
        const x = c.x * this.scale, y = c.y * this.scale;
        // the recognition and the first update come with the same event: one sample, or the pause detector reads
        // two zero speeds as a stop
        const t = Clutter.get_current_event_time() || GLib.get_monotonic_time() / 1000;
        const fresh = t > this._lastT;
        if (fresh) {
            this._tracker.add(t, x, y);
            this._lastT = t;
        }
        const dx = x - this._down.x, dy = y - this._down.y, up = Math.max(0, -dy);
        if (g.home) {
            this._moveHome(x, y, up, fresh);
            return;
        }
        g.up = up;
        g.likelyQuickSwitch = Math.atan2(up, Math.abs(dx)) * 180 / Math.PI <= QUICKSWITCH_DEG;
        this._pause.disallow = up < PAUSE_MIN_UP || g.likelyQuickSwitch;
        if (fresh)
            this._pause.add(this._tracker.velocityY / 1000);
        // the other tasks beside the app from the pause on, or for as long as the swipe runs along the bar
        if (this._pause.everPaused || g.likelyQuickSwitch)
            g.recents.attach();
        else
            g.recents.detach();
        // RecentsView pages under the horizontal motion, the whole swipe long
        g.recents.pagerMove(x - g.x, (g.rect[2] - g.rect[0]) / TASK_W);
        g.x = x;
        this._apply(clamp((-dy - TOUCH_SLOP) / DRAG_LENGTH, 0, MAX_SHIFT));
    }

    // TaskViewSimulator: the screen lerps into the recents card with shift, the corners to 84 px; past the card the
    // overview resistance shrinks it further and lifts it, the corners staying 84 px on screen
    _apply(shift) {
        const g = this._g;
        g.shift = shift;
        let rect, r;
        if (shift <= 1) {
            rect = lerpRect([0, 0, SCREEN_W, SCREEN_H], TASK, shift);
            r = TASK_CORNER_R * shift;
        } else {
            const u = (shift - 1) / (MAX_SHIFT - 1);
            const prog = u <= 0.134 ? u : 0.134 + DECELERATE((u - 0.134) / 0.866) * 0.134;
            const s = lerp(1, 1 - 0.001072 * TASK[3], prog);
            const ty = RESIST_TRANS_END * u;
            rect = [
                TASK_PIVOT[0] + (TASK[0] - TASK_PIVOT[0]) * s,
                TASK_PIVOT[1] + (TASK[1] - TASK_PIVOT[1]) * s + ty,
                TASK_PIVOT[0] + (TASK[2] - TASK_PIVOT[0]) * s,
                TASK_PIVOT[1] + (TASK[3] - TASK_PIVOT[1]) * s + ty,
            ];
            r = TASK_CORNER_R;
        }
        g.rect = rect;
        g.r = r;
        g.recents.follow(rect, r);
    }

    _paused() {
        if (this._g?.home)
            this._homeToOverview();
        else
            this._g?.recents.attach();
    }

    _release(cancelled) {
        const g = this._g;
        if (!g)
            return;
        // VelocityTracker on ACTION_UP: a lift-off 40 ms after the last move means the finger had stopped
        const t = Clutter.get_current_event_time();
        if (t && t - this._lastT > 40)
            this._tracker.clear();
        if (g.home) {
            this._releaseHome(cancelled);
            return;
        }
        const v = {x: this._tracker.velocityX / 1000, y: this._tracker.velocityY / 1000};
        const paused = this._pause.paused;
        this._pause.reset();
        const {recents} = g;
        // RecentsView has the ACTION_UP first: the page it snaps to decides as much as the swipe does
        const slop = recents.pagerDragged, centred = recents.centredPage();
        let page = recents.pagerRelease(cancelled ? 0 : v.x * 1000);
        if (cancelled) {
            page = 0;
            recents.snapTo(0, MAX_SWIPE_MS);
        }
        const scrollingNew = page !== 0, sideways = Math.abs(v.x) > Math.abs(v.y);
        let target;
        const fling = !cancelled && !paused && Math.abs(v.y) > FLING_PX_MS;
        if (cancelled)
            target = Target.LAST_TASK;
        else if (fling && v.y < 0)
            target = scrollingNew && sideways ? Target.NEW_TASK : Target.HOME;
        else if (fling)
            target = (scrollingNew && sideways) || centred !== 0 ? Target.NEW_TASK : Target.LAST_TASK;
        else if (scrollingNew && Math.abs(v.x) > FLING_PX_MS && slop)
            target = Target.NEW_TASK;
        else if (paused)
            target = Target.RECENTS;
        else if (scrollingNew)
            target = Target.NEW_TASK;
        else if (v.y < 0 && g.up >= PAUSE_MIN_UP)
            target = Target.HOME;
        else
            target = Target.LAST_TASK;

        if (target === Target.HOME) {
            recents.detach();
            this._flyHome(v);
        } else if (target === Target.RECENTS) {
            // OVERSHOOT_1_2 into the card, the scrim coming up LINEAR on the same clock, the strip to the nearest card
            if (page !== centred)
                recents.snapTo(centred, SETTLE_MS);
            this._settle(1, null, OVERSHOOT_1_2, () => {
                this._g = null;
                recents.show();
            }, SETTLE_MS, t => recents.setScrim(LINEAR(t)));
        } else if (target === Target.NEW_TASK) {
            // the shift back to full screen; once the strip is there too, the snapped task is the app (raised now,
            // under its card, so nothing is missing when the cards go)
            if (!scrollingNew) {
                page = centred;
                recents.snapTo(page, PAGER_SNAP_MS);
            }
            const win = recents.task(page)?.win;
            if (!win) {
                this._settle(0, null, DECELERATE, () => this._backToApp());
                return;
            }
            if (win.minimized)
                Main.wm.skipNextEffect(win.get_compositor_private());
            win.activate(global.get_current_time());
            this._settle(0, fling ? v : null, DECELERATE, () => recents.afterScroll(() => this._backToApp()));
        } else {
            recents.detach();
            const ms = this._settle(0, fling ? v : null, DECELERATE, () => recents.afterScroll(() => this._backToApp()));
            if (scrollingNew)
                recents.snapTo(0, ms);
        }
    }

    // AbsSwipeUpHandler's shift animation to `end`
    _settle(end, v, interpolator, done, fixedMs = 0, onProgress = null) {
        const g = this._g;
        let start = g.shift, ms;
        if (v) {
            start = clamp(start - v.y * FRAME_MS / DRAG_LENGTH, 0, MAX_SHIFT);
            ms = Math.min(MAX_SWIPE_MS, 2 * Math.abs((end - start) * DRAG_LENGTH / v.y));
        } else {
            ms = Math.min(MAX_SWIPE_MS, Math.abs(end - start) * MAX_SWIPE_MS * DURATION_MULT);
        }
        if (fixedMs)
            ms = fixedMs;
        this._anim = animate(g.recents.layer, 'settle', (_dt, elapsed) => {
            const t = ms > 0 ? clamp(elapsed * 1000 / ms, 0, 1) : 1;
            this._apply(lerp(start, end, interpolator(t)));
            onProgress?.(t);
            return t < 1;
        }, () => {
            this._anim = null;
            done();
        });
        return ms;
    }

    // the strip's app is the app again: the one the swipe started over, or the one it paged to
    _backToApp() {
        if (!this._g)
            return;
        this._g.recents.destroy();
        this._g = null;
        this.launcherState('normal');
        this.statusBarOverApp(null);
    }

    // RectFSpringAnim from the window's rect into its icon (or the icon-sized square above the dock when it has
    // none), with ScalingWorkspaceRevealAnim on the launcher from the same moment
    _flyHome(v) {
        const g = this._g;
        const task = g.recents.takeRunning();
        this.flight(g.win, task, task.rect ?? g.rect, g.r, v, {state: 'reveal'}, () => {
            g.recents.destroy();
            this._g = null;
        });
    }

    // The window, shown by task (a TaskView) at start with corners r on screen, flies into its icon (or the
    // icon-sized square above the dock when it has none) on RectFSpringAnim's springs from velocity v (px/ms), the
    // launcher playing `state` from the same moment. cropTop: the window keeps its top as it narrows (predictive
    // back), not its middle.
    flight(win, task, start, r, v, {state, cropTop = false}, done) {
        const app = Shell.WindowTracker.get_default().get_window_app(win);
        const known = app ? this.icons.get(app.get_id()) : null;
        const icon = known?.rect[2] > 0 ? known : null;
        const t0 = GLib.get_monotonic_time();
        this.launcherState(state, t0, icon ? app.get_id() : '');

        // the app goes to recents and the launcher takes the focus now; the clone flies on
        if (task.actor)
            Main.wm.skipNextEffect(task.actor);
        win.minimize();
        this.home.window?.activate(global.get_current_time());
        this.statusBarOverApp(null);

        const startW = start[2] - start[0], startH = start[3] - start[1];
        const r0 = r / (startW / SCREEN_W);            // the start radius in window px
        // the icon moves while the home scales in, so the target is re-read every frame
        const target = elapsed => {
            if (!icon) {
                const [cx, cy] = HOME_DEFAULT_CENTER, h = HOME_ICON_PX / 2;
                return [cx - h, cy - h, cx + h, cy + h];
            }
            const s = lerp(REVEAL_SCALE_FROM, 1, SCALE_REVEAL(clamp(elapsed / REVEAL_MS, 0, 1)));
            const [x, y, w, h] = icon.rect;
            return [x, y, x + w, y + h].map((c, i) => REVEAL_PIVOT[i % 2] + (c - REVEAL_PIVOT[i % 2]) * s);
        };
        const end0 = target(0);
        // an icon in the dock, or below the top third, is followed by its centre; higher up by its top
        const byTop = icon && end0[3] <= DOCK_TOP && end0[1] < SCREEN_H / 3;
        const ty = r => byTop ? r[1] : (r[1] + r[3]) / 2;
        const X = new Spring(HOME_X, (start[0] + start[2]) / 2, adjustVelocity(v.x * 1000, HOME_VX),
            (end0[0] + end0[2]) / 2);
        const Y = new Spring(HOME_Y, ty(start), adjustVelocity(v.y * 1000, HOME_VY), ty(end0));
        const P = new Spring(HOME_P, 0, 0, 1, 1 / startH, 1);
        const floating = icon ? new FloatingIcon(app, icon.path, this.scale, task.root) : null;

        this._anim = animate(task.root, 'home', (dt, elapsed) => {
            const end = target(elapsed * 1000);
            X.animateTo((end[0] + end[2]) / 2);
            Y.animateTo(ty(end));
            // sub-steps for the stiff springs over a long frame
            for (let left = Math.min(dt, 0.1); left > 0; left -= 0.004) {
                const h = Math.min(left, 0.004);
                X.step(h);
                Y.step(h);
                P.step(h);
            }
            const p = P.x;
            const w = lerp(startW, end[2] - end[0], p), h = lerp(startH, end[3] - end[1], p);
            const top = byTop ? Y.x : Y.x - h / 2;
            const rect = [X.x - w / 2, top, X.x + w / 2, top + h];
            const r = lerp(r0, SCREEN_W / 2, p) * w / SCREEN_W;
            const alpha = p < HOME_WIN_ALPHA_END ? 1 - Math.pow(p / HOME_WIN_ALPHA_END, 3) : 0;
            task.place(rect, r, alpha, cropTop);
            floating?.place(rect, r, p);
            if (capturing())
                log(`gnome-phone: home ${Math.round(elapsed * 1000)} p=${p.toFixed(3)} a=${alpha.toFixed(3)} ` +
                    `rect=${rect.map(Math.round)} r=${Math.round(r)}`);
            return !(X.done && Y.done && P.done);
        }, () => {
            this._anim = null;
            floating?.lift();
            done();
            this._showIcon(floating);
        });
    }

    // The launcher's icon back. The flying one stays over it until the launcher's first frame with it is on screen
    // (it answers then): dropped at once, a frame or two had neither, and the icon blinked.
    _showIcon(floating) {
        Gio.DBus.session.call('de.yesman.NeoLauncher', '/de/yesman/NeoLauncher', 'de.yesman.NeoLauncher', 'ShowIcon',
            null, null, Gio.DBusCallFlags.NO_AUTO_START, SHOW_ICON_WAIT_MS, null, (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch {}
                floating?.destroy();
            });
    }

    // ---- from the home screen --------------------------------------------------------------------------------------

    _beginHome() {
        const recents = this._recents = new Recents(this, null);
        this._g = {home: true, recents, p: 0, entering: false, reached: false, released: false, anim: null};
        this._move();
    }

    // the launcher's workspace and dock, frame by frame
    _pose(scale, alpha, depth, ty = 0) {
        this.launcher('Pose', new GLib.Variant('(dddd)', [scale, alpha, depth, ty]));
    }

    // NORMAL → HINT_STATE, linear in p: the workspace 1 → 0.92, the depth 0 → 0.05, the scrim up to HintState's
    _hint(p) {
        this._g.p = p;
        this._pose(lerp(1, HINT_SCALE, p), 1, HINT_DEPTH * p);
        this._g.recents.setScrim(HINT_SCRIM * p);
    }

    _moveHome(x, y, up, fresh) {
        const g = this._g;
        g.x = x;
        g.y = y;
        this._pause.disallow = up < HOME_PAUSE_MIN_UP;
        if (fresh)
            this._pause.add(this._tracker.velocityY / 1000);
        if (!g.entering) {
            // the hint tracks the finger over the whole height of the screen
            this._hint(clamp(up / SCREEN_H, 0, 1));
        } else if (!g.reached) {
            g.from = {x, y};
        } else {
            // there: the cards follow the finger a quarter of the way; up, the overview's resistance instead
            const tx = (x - g.from.x) * OVERVIEW_MOVEMENT, rise = (g.from.y - y) / g.from.y;
            if (rise > 0)
                g.recents.drag(tx, 0, rise);
            else
                g.recents.drag(tx, (y - g.from.y) * OVERVIEW_MOVEMENT, 0);
        }
    }

    // The pause: HINT → OVERVIEW in 380 ms, on OVERSHOOT_1_2 nearly throughout. The workspace shrinks to a card's
    // width, fades and rises (ACCELERATE), the depth goes to the overview's, the cards come in from the side.
    _homeToOverview() {
        const g = this._g;
        g.entering = true;
        g.from = {x: g.x, y: g.y};
        const s0 = lerp(1, HINT_SCALE, g.p), d0 = HINT_DEPTH * g.p;
        g.recents.enterFromHome(OVERVIEW_IN_MS, () => {
            g.reached = true;
            this._homeSettle();
        });
        g.anim = animate(g.recents.layer, 'hint-overview', (_dt, elapsed) => {
            const t = clamp(elapsed * 1000 / OVERVIEW_IN_MS, 0, 1), o = OVERSHOOT_1_2(t);
            this._pose(lerp(s0, OVERVIEW_WS_SCALE, o), clamp(1 - o, 0, 1), lerp(d0, OVERVIEW_DEPTH, o),
                OVERVIEW_WS_TY * ACCELERATE(t));
            return t < 1;
        }, () => {
            g.anim = null;
            this.launcherState('background');
        });
    }

    // there, and let go (in either order): the cards settle back under the finger's quarter, and the overview is up
    _homeSettle() {
        const g = this._g;
        if (!g?.home || !g.reached || !g.released)
            return;
        this._g = null;
        g.recents.settleDrag(() => g.recents.show());
    }

    _releaseHome(cancelled) {
        const g = this._g;
        const vy = cancelled ? 0 : this._tracker.velocityY / 1000;
        const paused = this._pause.paused;
        this._pause.reset();
        g.released = true;
        if (!g.entering) {
            this._hintRelease(vy);
        } else if (!paused || cancelled) {
            // moving again when let go: OverviewToHomeAnim
            g.anim?.stop();
            this._g = null;
            g.recents.goHome(vy < 0 ? 'home-reveal' : 'home');
        } else {
            this._homeSettle();
        }
    }

    // Let go before a pause. A fling up (or past half the screen) finishes the hint in its 80 ms and HINT → NORMAL
    // springs the workspace back; otherwise it goes straight back, in the time the swipe detector gives
    _hintRelease(vy) {
        const g = this._g, p0 = g.p;
        const fling = Math.abs(vy) > HOME_FLING;
        const toHint = fling ? vy < 0 : p0 > 0.5;
        const curve = Math.abs(vy) > 10 ? SCROLL : SCROLL_CUBIC;
        const start = clamp(p0 - vy * FRAME_MS / SCREEN_H, 0, 1);
        const finish = () => {
            g.anim = null;
            this.launcherState('normal');
            g.recents.destroy();
            this._g = null;
        };
        const tween = (ms, to, done) => {
            g.anim = animate(g.recents.layer, 'hint', (_dt, elapsed) => {
                const t = clamp(elapsed * 1000 / ms, 0, 1);
                this._hint(lerp(start, to, curve(t)));
                return t < 1;
            }, done);
        };
        if (toHint)
            tween(HINT_MS, 1, () => this._hintToNormal(finish));
        else
            tween(Math.max(100, 1200 / Math.max(2, Math.abs(0.5 * vy)) * Math.max(0.2, p0)), 0, finish);
    }

    // HINT → NORMAL: the scale on the hint spring; the depth (DECELERATE_3) and the scrim over its settle time
    _hintToNormal(done) {
        const g = this._g;
        const spring = new Spring(HINT_SPRING, HINT_SCALE, HINT_SPRING_V, 1, 1 / 500);
        const probe = new Spring(HINT_SPRING, HINT_SCALE, HINT_SPRING_V, 1, 1 / 500);
        let ms = 0;
        for (; !probe.done && ms < 2000; ms += 4)
            probe.step(0.004);
        g.anim = animate(g.recents.layer, 'hint-normal', (dt, elapsed) => {
            spring.advance(dt);
            const t = clamp(elapsed * 1000 / ms, 0, 1);
            this._pose(spring.x, 1, HINT_DEPTH * (1 - DECELERATE_3(t)));
            g.recents.setScrim(HINT_SCRIM * (1 - t));
            return !(spring.done && t >= 1);
        }, done);
    }

    destroy() {
        if (this._warmId)
            GLib.source_remove(this._warmId);
        this._anim?.stop();
        this._anim = null;
        this._g?.anim?.stop();
        this._g = null;
        this._recents?.destroy();
        this._recents = null;
        forgetClosing();
        if (this._keyguardId)
            this._keyguard.disconnect(this._keyguardId);
        this._keyguard = null;
        this._pause.destroy();
        this._back.destroy();
        this._sideways.destroy();
        dropKeptViews();
        this._keyboard?.run_dispose();
        global.stage.remove_action(this._gesture);
        this._gesture.disconnectObject(this);
        if (this._edgeDrag)
            this._edgeDrag.enabled = true;
        if (this._homeBar) {
            this._homeBar.show();
            Main.layoutManager.trackChrome(this._homeBar, {affectsStruts: true, trackFullscreen: true});
        }
        this._homeBar = null;
    }
}
