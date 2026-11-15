// App launch from a launcher icon, 1:1 with Launcher3's QuickstepTransitionManager on Android 16: the window grows
// out of the icon's box in 500 ms, its crop opening from a 160 dp square to the whole window, its corners from round
// to square, x on APP_OPEN_X and the rest on EMPHASIZED. A cold start shows a splash meanwhile (the app's icon on a
// flat background, the Android 12 starting window) that leaves with a radial vanish from the top centre while the
// window slides up 20 dp under it.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {capture, capturing} from './capture.js';

const LAUNCH_MS = 500;
const START_DELAY_MS = 16;
const CROP_START_DP = 160;              // starting_surface_icon_size
const ICON_ALPHA_DELAY_MS = 25;         // the floating icon of a warm launch fades out in 25..75 ms
const ICON_ALPHA_MS = 50;

const SPLASH_ICON_FADE_MS = 133;
const SPLASH_REVEAL_DELAY_MS = 83;
const SPLASH_REVEAL_MS = 266;
const SPLASH_EXIT_MS = 349;
const SPLASH_SHIFT_DP = 20;
const ATTENTION_MS = 10000;             // how long a launched app's windows are its launch's, not GNOME's banner
const INTENT_MS = 3000;                 // a window asking within this of a tap or key was asked for by the user
const SPLASH_SOLID = 0.8;               // the vanish gradient is opaque to 0.8 R
const SPLASH_TIMEOUT_MS = 15000;        // no window by then: the splash goes
const SPLASH_RGB = [47, 47, 47];        // dark theme starting window, measured
const FAIL_FADE_MS = 150;               // the splash of an app that cannot start

const ANDROID_DENSITY = 3;              // px per dp on the S10e

// ---- curves -------------------------------------------------------------------------------------------------------

// android.view.animation.PathInterpolator for a path of cubic segments ("M0,0 C… …")
export class PathInterpolator {
    constructor(path) {
        const n = path.match(/-?[\d.]+/g).map(Number);
        this._segs = [];
        let x0 = n[0], y0 = n[1];
        for (let i = 2; i + 5 < n.length; i += 6) {
            this._segs.push([x0, y0, n[i], n[i + 1], n[i + 2], n[i + 3], n[i + 4], n[i + 5]]);
            x0 = n[i + 4];
            y0 = n[i + 5];
        }
    }

    get(x) {
        if (x <= 0)
            return 0;
        if (x >= 1)
            return 1;
        const s = this._segs.find(g => x <= g[6]) ?? this._segs[this._segs.length - 1];
        const at = (t, a, b, c, d) => {
            const u = 1 - t;
            return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
        };
        let lo = 0, hi = 1;
        for (let i = 0; i < 30; i++) {
            const mid = (lo + hi) / 2;
            if (at(mid, s[0], s[2], s[4], s[6]) < x)
                lo = mid;
            else
                hi = mid;
        }
        return at((lo + hi) / 2, s[1], s[3], s[5], s[7]);
    }
}

export const cubic = (x1, y1, x2, y2) => new PathInterpolator(`M0,0 C${x1},${y1} ${x2},${y2} 1,1`);

const EMPHASIZED = new PathInterpolator('M0,0 C0.05,0 0.133333,0.06 0.166666,0.4 C0.208333,0.82 0.25,1 1,1');
const APP_OPEN_X = new PathInterpolator('M0,0 C0.1217,0.0462 0.15,0.4686 0.1667,0.66 C0.1834,0.8878 0.1667,1 1,1');
const ICON_FADE = cubic(0.15, 0, 1, 1);
const REVEAL_RADIUS = cubic(0, 0, 0.4, 1);
const ALPHA_OUT = cubic(0, 0, 0.8, 1);
const SHIFT = cubic(0, 0, 0, 1);

const lerp = (a, b, t) => a + (b - a) * t;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// ---- the crop ------------------------------------------------------------------------------------------------------

const DECLARATIONS = `
uniform vec4 bounds;   // the effect's texture in actor coordinates: x, y, width, height
uniform vec4 crop;     // x0, y0, x1, y1
uniform float radius;
uniform float aa;      // actor units per screen pixel
uniform vec4 hole;     // centre x, y, radius, strength: the splash's radial vanish
uniform float hole_top;
uniform float alpha;
`;

const CODE = `
vec2 p = bounds.xy + cogl_tex_coord_in[0].xy * bounds.zw;
vec2 c = (crop.xy + crop.zw) * 0.5;
vec2 q = abs(p - c) - (crop.zw - crop.xy) * 0.5 + vec2(radius);
float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
float cov = clamp(0.5 - d / aa, 0.0, 1.0);
if (hole.z > 0.0 && p.y >= hole_top) {
    float m = 1.0 - clamp((length(p - hole.xy) - ${SPLASH_SOLID.toFixed(2)} * hole.z) /
                          (${(1 - SPLASH_SOLID).toFixed(2)} * hole.z), 0.0, 1.0);
    cov *= 1.0 - hole.w * m;
}
cogl_color_out *= cov * alpha;
`;

// The window (or splash) through a rounded crop: what Android's SurfaceControl crop + corner radius do
export const CropEffect = GObject.registerClass(
class CropEffect extends Clutter.ShaderEffect {
    vfunc_get_static_snippet() {
        return Cogl.Snippet.new(Cogl.SnippetHook.FRAGMENT, DECLARATIONS, CODE);
    }

    // Where the offscreen texture lies in actor coordinates: the actor's paint box, enlarged the way
    // _clutter_actor_box_enlarge_for_effects does (whole pixels, 3 px of slack)
    setGeometry(actor, crop, radius, aa) {
        let x = 0, y = 0, [w, h] = actor.get_size();
        const pv = actor.get_paint_volume();
        if (pv) {
            const o = pv.get_origin();
            x = o.x;
            y = o.y;
            w = pv.get_width();
            h = pv.get_height();
        }
        const x2 = Math.ceil(x + w + 0.75), y2 = Math.ceil(y + h + 0.75);
        const rw = Math.round(w), rh = Math.round(h);
        this.set_uniform_float('bounds', 4, [x2 - rw - 3, y2 - rh - 3, rw + 3, rh + 3]);
        this.set_uniform_float('crop', 4, crop);
        this.set_uniform_float('radius', 1, [radius]);
        this.set_uniform_float('aa', 1, [aa]);
        this.queue_repaint();
    }

    setHole(x, y, r, strength, top) {
        this.set_uniform_float('hole', 4, [x, y, r, strength]);
        this.set_uniform_float('hole_top', 1, [top]);
        this.queue_repaint();
    }

    // The window's alpha. Kept here, not in the actor's opacity: with this effect on the actor, Clutter keeps
    // painting at the opacity of the first frame that had one below 255
    setAlpha(a) {
        this.set_uniform_float('alpha', 1, [a]);
        this.queue_repaint();
    }
});

// ---- the transition -----------------------------------------------------------------------------------------------

export class LaunchTransition {
    constructor(boost) {
        this._boost = boost;
        this._running = null;
        this._launched = null;
        this._launchedAt = 0;
        // GNOME's "“Window” is ready" banner (WindowAttentionHandler): a cold start's window maps without focus
        // (mutter's focus-stealing prevention), the banner showed, and it went again as the window took focus a
        // moment later. Android has nothing of the kind, so a window of the app just launched gets none; any other
        // window that asks for attention still gets GNOME's.
        this._attention = Main.windowAttentionHandler;
        global.display.disconnectObject(this._attention);
        global.display.connectObject(
            'window-demands-attention', (display, win) => this._onAttention(display, win),
            'window-marked-urgent', (display, win) => this._onAttention(display, win),
            this);
    }

    _onAttention(display, win) {
        const app = win ? Shell.WindowTracker.get_default().get_window_app(win) : null;
        if (app && app === this._launched && GLib.get_monotonic_time() - this._launchedAt < ATTENTION_MS * 1000) {
            // under the splash it is focused as the splash goes; after it, now
            if (this._running?.app !== app && !win.has_focus())
                Main.activateWindow(win);
            return;
        }
        // One app opening another's screen (the call log's contact, Contacts' Send SMS) is Android's intent: the
        // screen comes to the front. GTK asks for its activation token as the finger lifts, when mutter no longer
        // counts the touch, so the window only asked for attention and GNOME put up "“Contacts” is ready" with
        // the screen pushed behind it, once per tap (owner, 11-10). Right after a tap or key it is the user's.
        if (win && !win.has_focus() &&
            display.get_current_time_roundtrip() - display.get_last_user_time() < INTENT_MS) {
            Main.activateWindow(win);
            return;
        }
        this._attention._onWindowDemandsAttention(display, win);
    }

    _scale() {
        return global.display.get_monitor_scale(Main.layoutManager.primaryIndex);
    }

    // Android dp as stage px
    _dp(v) {
        return v * ANDROID_DENSITY / this._scale();
    }

    // app: Shell.App; from: the icon's box on screen in device pixels; icon: the launcher's own picture of the
    // icon (a file), or '' for the app's themed icon
    launch(app, from, icon) {
        this._boost?.launch();
        this._finish();
        this._launched = app;
        this._launchedAt = GLib.get_monotonic_time();
        const scale = this._scale();
        const box = {x: from[0] / scale, y: from[1] / scale, w: from[2] / scale, h: from[3] / scale};
        const win = this._lastWindow(app);
        if (win)
            this._warm(app, win, box, icon);
        else
            this._cold(app, box, icon);
    }

    // a window of app's the shell opened without a launch to animate (Settings from a recents card's menu): raised
    // as it asks for attention, as a launched app's is
    expect(app) {
        this._launched = app;
        this._launchedAt = GLib.get_monotonic_time();
    }

    _lastWindow(app) {
        const wins = app.get_windows().filter(w => w.get_window_type() === Meta.WindowType.NORMAL);
        return wins.length ? wins[0] : null;
    }

    _iconActor(app, icon, size) {
        const gicon = icon ? new Gio.FileIcon({file: Gio.File.new_for_path(icon)}) : app.get_icon();
        return new St.Icon({gicon, icon_size: size, style: 'padding: 0;'});
    }

    // One frame of the grow: the crop, scale, place and corners of `actor`, whose `frame` (actor coordinates) is
    // the window's own rectangle, at linear progress p
    _apply(run, p) {
        const e = EMPHASIZED.get(p), ex = APP_OPEN_X.get(p);
        const {box, target, actor, frame, effect} = run;
        const crop0 = this._dp(CROP_START_DP);
        const iconScale = lerp(1, Math.min(target.width, target.height) / box.w, e);
        const iconW = box.w * iconScale, iconH = box.h * iconScale;
        const cropW = lerp(Math.min(crop0, target.width), target.width, e);
        const cropH = lerp(Math.min(crop0, target.height), target.height, e);
        const s = Math.min(1, Math.max(iconW / cropW, iconH / cropH));
        const cx = box.x + box.w / 2 + (target.x + target.width / 2 - box.x - box.w / 2) * ex;
        const cy = box.y + box.h / 2 + (target.y + target.height / 2 - box.y - box.h / 2) * e;

        // scale about the frame's centre, and that centre moved to (cx, cy)
        const fcx = frame.x + frame.width / 2, fcy = frame.y + frame.height / 2;
        const [aw, ah] = actor.get_size();
        actor.set_pivot_point(aw ? fcx / aw : 0.5, ah ? fcy / ah : 0.5);
        actor.scale_x = actor.scale_y = s;
        const [ax, ay] = [actor.x, actor.y];
        actor.translation_x = cx - (ax + fcx);
        actor.translation_y = cy - (ay + fcy);

        const r = lerp(crop0 / 2, 0, e);     // r0 = half the square crop, r1 = 0 (the S10e's screen corners)
        effect.setGeometry(actor,
            [fcx - cropW / 2, fcy - cropH / 2, fcx + cropW / 2, fcy + cropH / 2], r, 1 / (s * this._scale()));
        return {cx, cy, iconW, iconH, s};
    }

    _reset(actor, effect) {
        actor.remove_effect(effect);
        actor.scale_x = actor.scale_y = 1;
        actor.translation_x = actor.translation_y = 0;
        actor.set_pivot_point(0, 0);
    }

    // A frame-clock driven run of `ms` from now, stretched by the shell's slow-down factor (St.Settings, set from
    // Looking Glass) like the shell's own animations
    _timeline(actor, ms, tick, done) {
        const slow = St.Settings.get().slow_down_factor;
        const timeline = new Clutter.Timeline({actor, duration: Math.round(ms * slow)});
        const name = capturing() ? `launch${this._captures = (this._captures ?? 0) + 1}` : null;
        timeline.connect('new-frame', () => {
            const t = timeline.get_elapsed_time() / slow;
            tick(t);
            if (name)
                capture(name, t);
        });
        timeline.connect('completed', () => done());
        timeline.start();
        return timeline;
    }

    // ---- a running app: its window grows, the floating icon fades out over it

    _warm(app, win, box, icon) {
        const actor = win.get_compositor_private();
        if (!actor) {
            Main.activateWindow(win);
            return;
        }
        Main.wm.skipNextEffect(actor);
        Main.activateWindow(win);
        const target = win.get_frame_rect();
        const buf = win.get_buffer_rect();
        const frame = {x: target.x - buf.x, y: target.y - buf.y, width: target.width, height: target.height};
        const effect = new CropEffect();
        effect.setHole(0, 0, 0, 0, 0);
        actor.add_effect_with_name('gnome-phone-launch', effect);
        const floating = this._iconActor(app, icon, box.w);
        global.window_group.add_child(floating);

        const run = {box, target, actor, frame, effect, floating};
        this._running = run;
        const step = elapsed => {
            const p = clamp((elapsed - START_DELAY_MS) / LAUNCH_MS, 0, 1);
            const g = this._apply(run, p);
            const iconAlpha = 1 - clamp((p * LAUNCH_MS - ICON_ALPHA_DELAY_MS) / ICON_ALPHA_MS, 0, 1);
            effect.setAlpha(1 - iconAlpha);
            floating.visible = iconAlpha > 0;
            floating.set_pivot_point(0.5, 0.5);
            floating.scale_x = g.iconW / box.w;
            floating.scale_y = g.iconH / box.h;
            floating.set_position(g.cx - box.w / 2, g.cy - box.h / 2);
            floating.opacity = Math.round(255 * iconAlpha);
        };
        step(0);
        run.timeline = this._timeline(actor, START_DELAY_MS + LAUNCH_MS, step, () => this._finish());
        run.finish = () => {
            run.timeline?.stop();
            floating.destroy();
            this._reset(actor, effect);
        };
    }

    // ---- a cold start: the splash grows, the window arrives under it, the splash vanishes

    _cold(app, box, icon) {
        const area = Main.layoutManager.getWorkAreaForMonitor(Main.layoutManager.primaryIndex);
        const splash = new Clutter.Actor({
            background_color: new Cogl.Color({red: SPLASH_RGB[0], green: SPLASH_RGB[1], blue: SPLASH_RGB[2],
                alpha: 255}),
            x: area.x, y: area.y, width: area.width, height: area.height,
            reactive: true,
        });
        const size = this._dp(CROP_START_DP);
        const iconActor = this._iconActor(app, icon, size);
        iconActor.set_position(Math.round((area.width - size) / 2), Math.round((area.height - size) / 2));
        splash.add_child(iconActor);
        global.window_group.add_child(splash);
        const effect = new CropEffect();
        effect.setAlpha(1);
        splash.add_effect_with_name('gnome-phone-launch', effect);

        const frame = {x: 0, y: 0, width: area.width, height: area.height};
        const run = {box, target: area, actor: splash, frame, effect, app, iconActor, grown: false, window: null};
        this._running = run;
        effect.setHole(0, 0, 0, 0, 0);
        const step = elapsed => this._apply(run, clamp((elapsed - START_DELAY_MS) / LAUNCH_MS, 0, 1));
        if (capturing())
            log(`gnome-phone: launch ${JSON.stringify({box, target: [area.x, area.y, area.width, area.height]})}`);
        step(0);
        run.timeline = this._timeline(splash, START_DELAY_MS + LAUNCH_MS, step, () => {
            run.timeline = null;
            run.grown = true;
            this._maybeExit(run);
        });

        // the app's first window: no map animation, hidden under the splash until it has drawn
        const tracker = Shell.WindowTracker.get_default();
        // A Wayland window is created before its app id is known: the tracker then gives a window-backed stand-in
        // ('window:N', no desktop file) and the real app only after notify::wm-class / gtk-application-id. Refusing
        // that stand-in left the splash waiting its whole timeout for every app (measured 2026-11-15: the window
        // drew after 0.4 s, the splash stayed 15 s). So claim it, and let go if the real owner is another app.
        const release = () => {
            if (!run.window)
                return;
            run.windowActor?.disconnectObject(run);
            if (run.windowActor)
                run.windowActor.opacity = 255;
            run.window.disconnectObject(run);
            run.window = run.windowActor = null;
        };
        global.display.connectObject('window-created', (_d, win) => {
            if (run.window || win.get_window_type() !== Meta.WindowType.NORMAL)
                return;
            const owner = tracker.get_window_app(win);
            if (owner && owner !== app && owner.get_app_info())
                return;
            run.window = win;
            const actor = win.get_compositor_private();
            Main.wm.skipNextEffect(actor);
            actor.opacity = 0;
            run.windowActor = actor;
            const recheck = () => {
                const now = tracker.get_window_app(win);
                if (now && now !== app && now.get_app_info())
                    release();
            };
            win.connectObject('notify::wm-class', recheck, 'notify::gtk-application-id', recheck, run);
            actor.connectObject('first-frame', () => {
                run.drawn = true;
                if (run.timeoutId)
                    GLib.source_remove(run.timeoutId);
                run.timeoutId = 0;
                this._maybeExit(run);
            }, run);
        }, run);
        const timeout = SPLASH_TIMEOUT_MS * St.Settings.get().slow_down_factor;
        run.timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeout, () => {
            run.timeoutId = 0;
            this._finish();
            return GLib.SOURCE_REMOVE;
        });
        run.finish = () => {
            run.timeline?.stop();
            if (run.timeoutId)
                GLib.source_remove(run.timeoutId);
            global.display.disconnectObject(run);
            run.windowActor?.disconnectObject(run);
            run.window?.disconnectObject(run);
            if (run.windowActor) {
                run.windowActor.opacity = 255;
                run.windowActor.translation_y = 0;
            }
            splash.destroy();
        };
        this._activate(app, run);
    }

    // A D-Bus activatable app is started through its bus name first: app.activate() only logs a service that cannot
    // start (masked: the GSM gateway masks Calls and Chatty), and the splash waited its whole timeout for a window
    // that never came, the gestures dead under it (2026-11-04)
    _activate(app, run) {
        if (!app.get_app_info()?.get_boolean('DBusActivatable')) {
            app.activate();
            return;
        }
        const name = app.get_id().replace(/\.desktop$/, '');
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'StartServiceByName', new GLib.Variant('(su)', [name, 0]), null, Gio.DBusCallFlags.NONE, -1, null,
            (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch (e) {
                    if (this._running === run)
                        this._failed(run, app, e);
                    return;
                }
                if (this._running === run)
                    app.activate();
            });
    }

    // the app cannot start: the splash fades where it got to, and a notification says why
    _failed(run, app, error) {
        Gio.DBusError.strip_remote_error(error);
        console.warn(`gnome-phone: ${app.get_id()} cannot start: ${error.message}`);
        run.timeline?.stop();
        run.timeline = null;
        run.exiting = true;
        const {actor: splash} = run;
        run.timeline = this._timeline(splash, FAIL_FADE_MS, t => {
            splash.opacity = Math.round(255 * (1 - clamp(t / FAIL_FADE_MS, 0, 1)));
        }, () => {
            run.timeline = null;
            this._finish();
        });
        // from the app itself, with its icon
        const source = new MessageTray.Source({title: app.get_name(), icon: app.get_icon()});
        Main.messageTray.add(source);
        source.addNotification(new MessageTray.Notification({
            source,
            title: `${app.get_name()} can't open`,
            body: /masked/.test(error.message) ? "It's turned off on this phone" : error.message,
        }));
    }

    // a swipe home or to recents over a splash still waiting for its window: the splash goes
    cancelSplash() {
        if (this._running && !this._running.exiting && this._running.app)
            this._finish();
    }

    // The splash exit (WM Shell's SplashScreenExitAnimation, type 0): the icon fades, a hole opens from the top
    // centre, the window slides up from 20 dp below
    _maybeExit(run) {
        if (!run.grown || !run.drawn || run.exiting)
            return;
        run.exiting = true;
        const {actor: splash, effect, iconActor, windowActor} = run;
        this._reset(splash, effect);
        splash.add_effect_with_name('gnome-phone-launch', effect);
        const [w, h] = splash.get_size();
        const R = Math.sqrt(h * h + (w / 2) * (w / 2)) / SPLASH_SOLID;
        const shift = this._dp(SPLASH_SHIFT_DP);
        windowActor.opacity = 255;
        windowActor.translation_y = shift;
        const step = t => {
            iconActor.opacity = Math.round(255 * (1 - ICON_FADE.get(clamp(t / SPLASH_ICON_FADE_MS, 0, 1))));
            const lp = clamp((t - SPLASH_REVEAL_DELAY_MS) / SPLASH_REVEAL_MS, 0, 1);
            const dy = shift * (1 - SHIFT.get(lp));
            windowActor.translation_y = dy;
            effect.setGeometry(splash, [0, 0, w, h], 0, 1 / this._scale());
            // the hole stops at the window's top while it slides: the splash colour fills the gap (the occluding
            // view WM Shell puts there)
            effect.setHole(w / 2, 0, R * REVEAL_RADIUS.get(lp), ALPHA_OUT.get(lp), dy);
        };
        step(0);
        run.timeline = this._timeline(splash, SPLASH_EXIT_MS, step, () => {
            run.timeline = null;
            this._finish();
            if (run.window && !run.window.has_focus())
                Main.activateWindow(run.window);
        });
    }

    _finish() {
        const run = this._running;
        if (!run)
            return;
        this._running = null;
        run.finish?.();
    }

    destroy() {
        this._finish();
        global.display.disconnectObject(this);
        const handler = this._attention;
        global.display.connectObject(
            'window-demands-attention', handler._onWindowDemandsAttention.bind(handler),
            'window-marked-urgent', handler._onWindowDemandsAttention.bind(handler),
            handler);
    }
}
