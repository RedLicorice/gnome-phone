// The notification shade, Android 16's legacy NotificationPanelViewController on a phone (spec §2). Pulled from the
// status bar it tracks the finger 1:1 over H = 2190 px; on release it flings open with FlingAnimationUtils and a
// 16 dp overshoot that springs back, or flings closed. Behind it the screen blurs and zooms out (depth controller);
// in front, the QQS and the notifications scrim, whose top (stackY) is also the QS clip edge. Dragged down once open,
// the QS expands; dragged up, the shade closes.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {animate, Animator, clamp, FAST_OUT_SLOW_IN, fling, lerp, MIN_FLING_VELOCITY, Spring,
    VelocityTracker} from './motion.js';
import {NotificationStack} from './notifications.js';
import {TickScale} from './ornament.js';
import {isRenaissance} from './theme.js';
import {Color, rgba, SHADE_PANEL} from './palette.js';
import {QQS_HEIGHT, QSPanel} from './qs.js';
import {wholeFrames} from './redraws.js';
import {px, SCALE, SCREEN_H} from './units.js';

// --- spec §2, screen px
const H = 2190;                         // the height the finger tracks over (measured)
const QS_TRAVEL = 1680;                 // the stack's way from the QQS (644) to below the screen, q 0 -> 1
const OVERSHOOT = 48;                   // 16 dp, at 4500 px/s and above; 20 % of it at rest
const OVERSHOOT_FULL_VELOCITY = 4500;
const SPRING_BACK_MS = 400;
const OPEN_NO_VELOCITY_MS = 350;
const QS_OPPOSITE_MS = 350;
const TOUCH_SLOP = 24;                  // 8 dp
const CUTOFF = 0.5;
const STACK_START_FRACTION = 0.5;
const SCRIM_TOP_PADDING = 48;           // the stack's first row sits 16 dp under the scrim top
const SCRIM_RADIUS = 96;                // 32 dp
const BLUR_MIN = 1;
const BLUR_MAX = 102;                   // 34 dp, SurfaceFlinger units
const BLUR_INTERACTION = 0.8;           // the rest is the spring
const BLUR_SPRING_STIFFNESS = 200;
const BLUR_VELOCITY_SCALE = 100;
const BLUR_VELOCITY_MAX = 3000;
const ZOOM_LAUNCHER = 0.05;
const ZOOM_APP = 0.025;
const MIRROR_MS = 150;                  // the shade fading away around the brightness slider

// ShadeInterpolation.kt: 0 below f 0.417, 0.164 at 0.5, 0.837 at 0.7
export function contentAlpha(f) {
    return ease(clamp((f - 0.3) / 0.7));
}

// ... and the notifications scrim's curve, which also drives the blur: 1 from f 0.5
export function notificationScrimAlpha(f) {
    return ease(clamp(f / 0.5));
}

function ease(m) {
    const x = 1.2 * m - 0.2;
    return x <= 0 ? 0 : 1 - 0.5 * (1 - Math.cos(Math.PI * (1 - x) ** 2));
}

const blurOfRatio = r => r === 0 ? 0 : lerp(BLUR_MIN, BLUR_MAX, r);
const ratioOfBlur = b => b === 0 ? 0 : clamp((b - BLUR_MIN) / (BLUR_MAX - BLUR_MIN));

export const Shade = GObject.registerClass({
    Signals: {'expansion-changed': {}},
}, class Shade extends St.Widget {
    constructor(state, statusBar) {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'gnomePhoneShade',
            reactive: true,
            visible: false,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
        });
        this._statusBar = statusBar;

        // behind: the blurred screen, then the shade panel tint (α 0.723 · f); drawn in whole frames while it shows
        // (redraws.js)
        this._blurLayer = new St.Widget({width: this.width, height: this.height});
        this._blur = new Shell.BlurEffect({mode: Shell.BlurMode.BACKGROUND, radius: 0, brightness: 1});
        this._blur.enabled = false;
        this._blurLayer.add_effect(this._blur);
        this.add_child(this._blurLayer);
        this._tint = new St.Widget({
            width: this.width,
            height: this.height,
            style: `background-color: ${rgba(SHADE_PANEL[0].map(Math.round), SHADE_PANEL[1])};`,
            opacity: 0,
        });
        this.add_child(this._tint);

        this._qs = new QSPanel(state, this);
        this.add_child(this._qs);

        // the notifications scrim: from stackY down, 32 dp top corners, accent1_800 α 0.5 · f
        const r = px(SCRIM_RADIUS).toFixed(2);
        this._scrim = new St.Widget({
            width: this.width,
            height: px(SCREEN_H + SCRIM_RADIUS),
            style: `background-color: ${rgba(Color.notificationScrim, 0.5)}; border-radius: ${r}px ${r}px 0 0;`,
            opacity: 0,
        });
        this.add_child(this._scrim);
        // the Renaissance look: a fine tick scale along the scrim's top edge, with a silver hairline above it
        if (isRenaissance()) {
            this._scrim.style += ` border-top: 1px solid ${rgba(Color.silverDim, 0.6)};`;
            this._scrim.add_child(new TickScale(this.width));
            this._scrim.get_last_child().set_position(0, px(6));
        }

        this._stack = new NotificationStack(this);
        this.add_child(this._stack);

        // expansion: f (0..1), the overshoot in px, the QS expansion q (0..1)
        this._f = 0;
        this._over = 0;
        this._q = 0;
        this._mirror = 0;
        this._stackY = 0;
        this._tracking = false;
        this._velocity = new VelocityTracker();

        // the depth controller's spring and its bookkeeping (NotificationShadeDepthController.kt:555-680)
        this._blurSpring = new Spring({stiffness: BLUR_SPRING_STIFFNESS, dampingRatio: 1, threshold: 0.5});
        this._isBlurred = false;
        this._isClosed = true;
        this._isOpen = false;
        this._prevDepthF = 0;
        this._prevDepthTime = -1;

        // pull from the status bar
        this._pull = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            max_n_points: 1,
            begin_threshold: px(TOUCH_SLOP),
        });
        // not while locked: an app's window over the lock screen (whenlocked.js) shows the status bar, not the shade
        this._pull.connect('may-recognize', () => !Main.sessionMode.isLocked);
        this._pull.connect('recognize', g => this._beginDrag(g, false));
        this._pull.connect('pan-update', g => this._updateDrag(g));
        this._pull.connect('end', () => this._endDrag(false));
        this._pull.connect('cancel', () => this._endDrag(true));
        statusBar.add_action(this._pull);
        // at shutdown the status bar may go first
        statusBar.connectObject('destroy', () => {
            this._statusBar = null;
        }, this);

        // drags on the open shade: down expands the QS, up collapses QS and then the shade
        this._pan = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            max_n_points: 1,
            begin_threshold: px(TOUCH_SLOP),
        });
        this._pan.connect('recognize', g => this._beginDrag(g, true));
        this._pan.connect('pan-update', g => this._updateDrag(g));
        this._pan.connect('end', () => this._endDrag(false));
        this._pan.connect('cancel', () => this._endDrag(true));
        this.add_action(this._pan);

        // a tap on nothing under the stack closes it
        this._tap = new Clutter.ClickGesture();
        this._tap.connect('recognize', g => {
            const point = g.get_point_coords(0);
            const target = global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE,
                this.x + point.x, this.y + point.y);
            if (target === this && point.y * SCALE > this._stackY)
                this.close();
        });
        this.add_action(this._tap);

        this.connect('destroy', () => this._onDestroy());
    }

    get expansion() {
        return this._f;
    }

    get isOpen() {
        return this._f > 0;
    }

    // how far it is pulled, screen px
    get pulledHeight() {
        return this._f * H;
    }

    // where the stack's first row is, screen px, and whether it shows @notification expanded
    get notificationsTop() {
        return this._stackY + SCRIM_TOP_PADDING;
    }

    rowExpanded(notification) {
        return this._stack.rowExpanded(notification);
    }

    // A pull from a heads-up notification: the shade starts as far open as the notification reaches down
    // (HeadsUpTouchHelper: startExpand(startHeight)), and the drag goes on from there
    beginPull(gesture, startHeight) {
        this._beginDrag(gesture, false, startHeight);
    }

    updatePull(gesture) {
        this._updateDrag(gesture);
    }

    endPull(cancelled) {
        this._endDrag(cancelled);
    }

    // --- dragging

    // the shade's position along one axis: f · H, then the QS's q · QS_TRAVEL (px)
    get _position() {
        return this._f * H + this._q * QS_TRAVEL;
    }

    _setPosition(p, maxP) {
        p = clamp(p, 0, maxP);
        this._f = Math.min(p, H) / H;
        this._q = Math.max(p - H, 0) / QS_TRAVEL;
        this._queueApply();
    }

    _beginDrag(gesture, open, startP = null) {
        // on the open shade a drag on a long stack scrolls it first (NPVC: canCollapsePanelOnTouch, QS expands
        // only from the stack's top)
        if (open && this._f === 1 && this._q === 0 && !this._anim) {
            const begin = gesture.get_begin_centroid_abs().y;
            const up = gesture.get_centroid_abs().y < begin;
            if (this._stack.canScroll(begin * SCALE, up)) {
                this._scrolling = true;
                this._dragStartY = gesture.get_centroid_abs().y;
                this._velocity.clear();
                this._trackVelocity(gesture);
                this._stack.beginScroll();
                return;
            }
        }
        this._stack.stopScroll();
        this._stopAnimation();
        this._tracking = true;
        this._dragOpen = open;
        this._dragStartP = startP ?? this._position;
        this._dragStartY = gesture.get_centroid_abs().y;
        this._velocity.clear();
        this._trackVelocity(gesture);
        if (!open) {
            this.visible = true;
            this._queueApply();
        }
    }

    _trackVelocity(gesture) {
        const event = gesture.get_point_event(0);
        const time = event ? event.get_time() : GLib.get_monotonic_time() / 1000;
        this._velocity.add(time, 0, gesture.get_centroid_abs().y * SCALE);
    }

    _updateDrag(gesture) {
        if (this._scrolling) {
            this._trackVelocity(gesture);
            this._stack.dragScroll((gesture.get_centroid_abs().y - this._dragStartY) * SCALE);
            return;
        }
        if (!this._tracking)
            return;
        this._trackVelocity(gesture);
        const dy = (gesture.get_centroid_abs().y - this._dragStartY) * SCALE;
        // a pull from the status bar stops at the QQS; on the open shade the drag reaches the full QS
        this._setPosition(this._dragStartP + dy, this._dragOpen ? H + QS_TRAVEL : H);
    }

    _endDrag(cancelled) {
        if (this._scrolling) {
            this._scrolling = false;
            if (!cancelled)
                this._stack.flingScroll(this._velocity.velocityY);
            return;
        }
        if (!this._tracking)
            return;
        this._tracking = false;
        const v = cancelled ? 0 : this._velocity.velocityY;
        if (this._q > 0) {
            const expand = Math.abs(v) >= MIN_FLING_VELOCITY ? v > 0 : this._q > CUTOFF;
            this._flingQs(expand, v);
        } else {
            const expand = Math.abs(v) >= MIN_FLING_VELOCITY ? v > 0 : this._f > CUTOFF;
            this._flingPanel(expand, v);
        }
    }

    // --- animations

    _stopAnimation() {
        this._anim?.stop();
        this._anim = null;
    }

    // NPVC.flingToHeight: open to H with an overshoot that depends on the velocity, or close
    _flingPanel(expand, v) {
        this._stopAnimation();
        const cur = this._f * H;
        const over0 = this._over;
        if (expand) {
            const addOverscroll = over0 === 0 && v >= 0;
            const amount = addOverscroll ? lerp(0.2, 1, clamp(v / OVERSHOOT_FULL_VELOCITY)) : 0;
            const springBack = addOverscroll || over0 !== 0;
            this._grab();
            if (cur === H && over0 === 0 && !addOverscroll) {
                this._settled();
                return;
            }
            let {duration, interpolator} = fling(cur, H + amount * OVERSHOOT, v,
                {maxLength: 0.4, speedUpFactor: 0, maxDistance: SCREEN_H});
            if (v === 0)
                duration = OPEN_NO_VELOCITY_MS;
            this._anim = animate(this, {
                from: cur, to: H, duration, interpolator,
                onUpdate: (h, fraction) => {
                    if (amount > 0)
                        this._over = lerp(over0, amount * OVERSHOOT, FAST_OUT_SLOW_IN(fraction));
                    this._f = h / H;
                    this._queueApply();
                },
                onDone: () => {
                    this._anim = null;
                    if (springBack)
                        this._springBack();
                    else
                        this._settled();
                },
            });
        } else {
            const {duration, interpolator} = fling(cur, 0, v,
                {maxLength: 0.6, speedUpFactor: 0.6, maxDistance: SCREEN_H});
            this._anim = animate(this, {
                from: cur, to: 0, duration, interpolator,
                onUpdate: (h, fraction) => {
                    if (over0 !== 0)
                        this._over = lerp(over0, 0, FAST_OUT_SLOW_IN(fraction));
                    this._f = h / H;
                    this._queueApply();
                },
                onDone: () => {
                    this._anim = null;
                    this._closed();
                },
            });
        }
    }

    // the overshoot back to 0 over 400 ms
    _springBack() {
        const over0 = this._over;
        this._anim = animate(this, {
            from: over0, to: 0, duration: SPRING_BACK_MS, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._over = v;
                this._queueApply();
            },
            onDone: () => {
                this._anim = null;
                this._settled();
            },
        });
    }

    // QuickSettingsControllerImpl.flingQs: FAU 0.4 s; against the velocity 350 ms
    _flingQs(expand, v) {
        this._stopAnimation();
        const cur = this._q * QS_TRAVEL, target = expand ? QS_TRAVEL : 0;
        const opposite = (v > 0 && !expand) || (v < 0 && expand);
        if (opposite)
            v = 0;
        let {duration, interpolator} = fling(cur, target, v, {maxLength: 0.4});
        if (opposite)
            duration = QS_OPPOSITE_MS;
        this._anim = animate(this, {
            from: cur, to: target, duration, interpolator,
            onUpdate: p => {
                this._f = 1;
                this._q = p / QS_TRAVEL;
                this._queueApply();
            },
            onDone: () => {
                this._anim = null;
                this._settled();
            },
        });
    }

    // open from code (no velocity: 350 ms)
    open() {
        if (this._tracking)
            return;
        this.visible = true;
        this._flingPanel(true, 0);
    }

    // close from code: the QS and the shade together, the closing curve at no velocity
    close() {
        if (this._tracking || this._f === 0)
            return;
        this._stopAnimation();
        const p0 = this._position, over0 = this._over;
        const {duration, interpolator} = fling(p0, 0, 0, {maxLength: 0.6, speedUpFactor: 0.6, maxDistance: SCREEN_H});
        this._anim = animate(this, {
            from: p0, to: 0, duration, interpolator,
            onUpdate: (p, fraction) => {
                if (over0 !== 0)
                    this._over = lerp(over0, 0, FAST_OUT_SLOW_IN(fraction));
                this._setPosition(p, H + QS_TRAVEL);
            },
            onDone: () => {
                this._anim = null;
                this._closed();
            },
        });
    }

    toggle() {
        if (this._f > 0)
            this.close();
        else
            this.open();
    }

    _settled() {
        this._queueApply();
    }

    _closed() {
        this._f = 0;
        this._q = 0;
        this._over = 0;
        this._ungrab();
        this._queueApply();
    }

    // --- modal while open: keys come here (Escape closes), the screen below takes no input

    _grab() {
        if (this._grabbed)
            return;
        this._grabbed = Main.pushModal(this, {actionMode: Shell.ActionMode.POPUP});
        this.grab_key_focus();
    }

    _ungrab() {
        if (!this._grabbed)
            return;
        Main.popModal(this._grabbed);
        this._grabbed = null;
    }

    // St gives the actor with key focus the :focus pseudo class, and that restyles every actor inside it: each icon
    // in the shade drew its picture again as the grab was taken, and again as it was given back. Nothing here is
    // styled by focus.
    vfunc_key_focus_in() {
    }

    vfunc_key_focus_out() {
    }

    vfunc_key_press_event(event) {
        const key = event.get_key_symbol();
        if (key === Clutter.KEY_Escape || key === Clutter.KEY_Back) {
            this.close();
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_PROPAGATE;
    }

    // --- the brightness mirror: while the slider is dragged, everything but the slider fades and the blur goes

    setBrightnessDragging(dragging) {
        this._mirrorAnim?.stop();
        const from = this._mirror, to = dragging ? 1 : 0;
        this._mirrorAnim = animate(this, {
            from, to, duration: MIRROR_MS * Math.abs(to - from), interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._mirror = v;
                this._queueApply();
            },
            onDone: () => {
                this._mirrorAnim = null;
            },
        });
    }

    onTileActivated(tile, longPress) {
        if (longPress)
            this.close();
    }

    // --- per frame

    _queueApply() {
        if (this._applyId)
            return;
        this._applyId = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._applyId = 0;
            this._apply();
            return GLib.SOURCE_REMOVE;
        });
    }

    _apply() {
        const f = this._f, q = this._q, over = this._over, m = this._mirror;
        const shown = f > 0;
        this.visible = shown || this._tracking;
        this._updateDepth(f);

        const stackY = lerp(0, QQS_HEIGHT + over, f) + q * QS_TRAVEL;
        this._stackY = stackY;
        const content = contentAlpha(f);

        // the tiles squish with the stack's expansion fraction without the shelf (StackScrollAlgorithm.java:606)
        const end = SCREEN_H - QQS_HEIGHT;
        const squish = clamp((lerp(STACK_START_FRACTION, 1, f) * end - SCRIM_TOP_PADDING) /
            (end - SCRIM_TOP_PADDING));

        this._tint.opacity = Math.round(255 * f * (1 - m));
        this._qs.layoutFor({q, squish, contentAlpha: content, mirror: m});
        this._qs.set_clip(0, 0, this.width, Math.max(px(stackY), 0));

        this._scrim.set_position(0, px(stackY));
        this._scrim.opacity = Math.round(255 * f * (1 - m));
        this._stack.layoutFor({stackY, ratio: squish, alpha: content * (1 - m)});

        this._applyBlur();
        this.emit('expansion-changed');
    }

    // NotificationShadeDepthController.updateShadeAnimationBlur: the spring part of the blur
    _updateDepth(f) {
        const now = GLib.get_monotonic_time() / 1e6;
        const dt = this._prevDepthTime < 0 ? 1 : clamp(now - this._prevDepthTime, 0.00001, 1);
        const diff = f - this._prevDepthF;
        if (diff === 0 && this._prevTracking === this._tracking) {
            this._prevDepthTime = now;
            return;
        }
        const direction = Math.sign(diff);
        const velocity = clamp(BLUR_VELOCITY_SCALE * diff / dt, -BLUR_VELOCITY_MAX, BLUR_VELOCITY_MAX);
        if (f > 0) {
            if (this._isClosed) {
                this._animateBlur(true, velocity);
                this._isClosed = false;
            }
            if (this._tracking && !this._isBlurred)
                this._animateBlur(true, 0);
            if (!this._tracking && direction < 0 && this._isBlurred)
                this._animateBlur(false, velocity);
            if (f === 1) {
                if (!this._isOpen) {
                    this._isOpen = true;
                    if (!this._isBlurred)
                        this._animateBlur(true, velocity);
                }
            } else {
                this._isOpen = false;
            }
        } else if (!this._isClosed) {
            this._isClosed = true;
            if (this._isBlurred)
                this._animateBlur(false, velocity);
        }
        this._prevDepthF = f;
        this._prevDepthTime = now;
        this._prevTracking = this._tracking;
    }

    _animateBlur(blur, velocity) {
        this._isBlurred = blur;
        this._blurSpring.target = blur ? BLUR_MAX : 0;
        this._blurSpring.velocity = velocity;
        if (this._blurAnim)
            return;
        this._blurAnim = new Animator(this, dt => {
            let left = Math.min(dt, 0.1);
            while (left > 0) {
                const h = Math.min(left, 0.004);
                this._blurSpring.step(h);
                left -= h;
            }
            this._queueApply();
            return !this._blurSpring.done;
        }, () => {
            this._blurAnim = null;
        });
    }

    _applyBlur() {
        const f = this._f;
        const animationRadius = clamp(this._blurSpring.value, BLUR_MIN, BLUR_MAX);
        const expansionRadius = blurOfRatio(notificationScrimAlpha(f));
        let radius = expansionRadius * BLUR_INTERACTION + animationRadius * (1 - BLUR_INTERACTION);
        radius = Math.max(radius, blurOfRatio(notificationScrimAlpha(this._q) * f));
        radius *= 1 - this._mirror;
        const blur = f > 0 ? Math.trunc(radius) : 0;

        this._blur.enabled = blur > 0;
        wholeFrames(this, blur > 0);
        if (blur > 0 && this._blur.radius !== blur)
            this._blur.radius = blur;

        // the screen behind zooms out with the blur: 5 % over the launcher, 2.5 % over an app
        const zoom = 1 - ratioOfBlur(blur) * (this._statusBar?.overApp ? ZOOM_APP : ZOOM_LAUNCHER);
        const group = global.window_group;
        if (zoom === 1) {
            group.set_scale(1, 1);
            group.set_translation(0, 0, 0);
        } else {
            // about the panel's centre: a stock shell's window group has no size of its own, so a relative pivot
            // would be its top left corner
            const m = Main.layoutManager.primaryMonitor;
            group.set_pivot_point(0, 0);
            group.set_scale(zoom, zoom);
            group.set_translation((m.x + m.width / 2) * (1 - zoom), (m.y + m.height / 2) * (1 - zoom), 0);
        }
    }

    _onDestroy() {
        wholeFrames(this, false);
        this._stopAnimation();
        this._blurAnim?.stop();
        this._mirrorAnim?.stop();
        if (this._applyId)
            global.compositor.get_laters().remove(this._applyId);
        this._ungrab();
        this._statusBar?.remove_action(this._pull);
        global.window_group.set_scale(1, 1);
        global.window_group.set_translation(0, 0, 0);
    }
});
