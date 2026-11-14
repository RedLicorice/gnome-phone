// The volume dialog: Android 16's redesigned VolumeDialog with the panel on the left, as LineageOS puts it (spec §5).
// A 60 dp pill slides in on a spring: the ringer button on top (feedbackd's profile), the media stream's slider and
// the settings button. The volume keys come here instead of to gnome-settings-daemon; as AudioService does, the first
// press only shows the panel and the next ones step the default output through 15 levels.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Gvc from 'gi://Gvc';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

import {raiseToTop} from './endmark.js';
import {vibrate} from './haptics.js';
import {Canvas, paintIcon, VectorIcon} from './icons.js';
import {animate, animateSpring, bezier, clamp, EMPHASIZED_DECELERATE, lerp, Spring} from './motion.js';
import {Color, gild, rgba, setSource} from './palette.js';
import {openSettings} from './settings.js';
import {px, SCALE, SCREEN_H, screenPixels} from './units.js';

// --- geometry, screen px (spec §5.3); the window runs from the status bar's bottom to the screen's
const ROOT_W = 528;                     // 176 dp: how far the panel slides
const WINDOW_TOP = 116;
const SLIDER_H = 762;                   // 254 dp, centred in the window
const S = (WINDOW_TOP + SCREEN_H) / 2 - SLIDER_H / 2;      // the slider's top: 817
const PILL_X = 24;                      // 8 dp from the edge, 60 dp wide
const PILL_W = 180;
const PILL_RADIUS = 90;
const BODY_TOP = S - 84;                // the main background starts 28 dp above the slider, square-topped
const SLIDER_X = 36;                    // 52 dp
const SLIDER_W = 156;
const TRACK_INSET_X = 18;               // the 40 dp track in the 52 dp slider
const TRACK_W = 120;
const TRACK_TOP = 6;                    // the track stops half a handle short of the slider's ends
const TRACK_LEN = 750;
const TRACK_RADIUS = 36;                // 12 dp, 2 dp next to the handle
const TRACK_INNER_RADIUS = 6;
const HANDLE_H = 12;                    // 52 x 4 dp
const HANDLE_GAP = 18;                  // 6 dp each side
const ICON_BOX = 120;                   // the stream icon: 20 dp in a 40 dp box
const ICON = 60;
const ICON_ON_ACTIVE_FROM = 0.816;      // past this the icon moves to the active track
const SECTION_GAP = 12;                 // 4 dp between the slider and the bottom section
const BUTTON = 144;                     // 48 dp buttons
const PILL_END_PAD = 18;                // 6 dp under the bottom section
const SETTINGS_TOP = S + SLIDER_H + SECTION_GAP;
const PILL_BOTTOM = SETTINGS_TOP + BUTTON + PILL_END_PAD;
const SETTINGS_RIPPLE = 120;            // ripple_drawable_20dp
const DOT_PITCH = 13;                   // audio_bars_idle: three dots, 4.375 dp apart
const DOT_RADIUS = 4.26;

// the ringer: a 60 dp cap above the slider; opened, a drawer of three 48 dp buttons, Normal at the bottom
const RINGER_BUTTON = 120;              // the 40 dp circle of a 48 dp button
const RINGER_ICON = 60;
const RINGER_SLOT = 144;
const RINGER_CENTER = S - 84;           // the bottom button's centre; a hidden one takes 1 dp
const RINGER_HIDDEN_STEP = 3;
const RINGER_BG_BOTTOM = S + 6;
const RINGER_BG_CLOSED = S - 174;       // 60 dp: the cap
const RINGER_BG_OPEN = S - 462;         // 48 x 3 + 12 dp
const RINGER_OPEN_RADIUS = 36;          // 12 dp
const SELECTED_RADIUS = 60;             // 20 dp
const UNSELECTED_RADIUS = 36;           // 12 dp
const RINGER_OPEN_MS = 400;
const RINGER_CLOSE_DELAY_MS = 300;
const RINGER_CLOSE = bezier(0.2, 0, 0, 1);

// --- behaviour (spec §5.5)
const LEVELS = 15;                      // STREAM_MUSIC
const SHOW_STIFFNESS = 700;
const SHOW_DAMPING = 0.9;
const VALUE_STIFFNESS = 1500;           // the slider's Animatable: StiffnessMedium, no bounce
const DISMISS_MS = 3000;
const SUPPRESS_MS = 500;                // LONG_PRESS_TIMEOUT: a held first press does not change the volume
const OVERSCROLL_MAX = 168;             // 56 dp
const OVERSCROLL = bezier(0.15, 0, 0.2, 1);
const OVERSCROLL_STIFFNESS = 800;
const OVERSCROLL_DAMPING = 0.6;
const ICON_IN_MS = 100;
const ICON_IN_DELAY_MS = 33;
const ICON_OUT_MS = 50;

// feedbackd's profiles stand in for Android's ringer modes; index is the drawer slot from the bottom
const RINGER_MODES = [
    {profile: 'full', icon: 'ic_volume_ringer', name: 'Ring'},
    {profile: 'silent', icon: 'ic_volume_ringer_mute', name: 'Mute'},
    {profile: 'quiet', icon: 'ic_volume_ringer_vibrate', name: 'Vibrate'},
];
const FEEDBACKD_SCHEMA = 'org.sigxcpu.feedbackd';

// --- haptics: SliderHapticFeedbackProvider with VolumeHapticsConfigsProvider.continuousConfigs, as it plays on the
// S10e under Android (no composition primitives there, so MSDL's fallbacks, read back with dumpsys vibrator_manager)
const DRAG_THRESHOLD = 0.02;            // deltaProgressForDragThreshold
const LOWER_BOOKEND = 0.01;             // SeekableSliderTrackerConfig
const UPPER_BOOKEND = 0.99;
const DRAG_TEXTURE = [[0.04, 10], [0.12, 20], [0.2, 20], [0.04, 10]];      // DRAG_INDICATOR_CONTINUOUS
const LIMIT_TICK = [[0.5, 5]];          // DRAG_THRESHOLD_INDICATOR_LIMIT: Prebaked TICK, MEDIUM = 0.5 for 5 ms
// both at twice Android's amplitude: the owner found them too subtle, as the pattern's dots (2026-11-09)
const twice = pattern => pattern.map(([amplitude, ms]) => [Math.min(1, amplitude * 2), ms]);

// a texture each time the progress has moved 2 % since the last one, a tick once on reaching either end
export class SliderHaptics {
    constructor() {
        this._last = -1;
        this._atLower = false;
        this._atUpper = false;
    }

    progress(p) {
        if (p <= LOWER_BOOKEND || p >= UPPER_BOOKEND) {
            const upper = p >= UPPER_BOOKEND;
            if (!(upper ? this._atUpper : this._atLower))
                vibrate(twice(LIMIT_TICK));
            this._atUpper = upper;
            this._atLower = !upper;
            return;
        }
        if (Math.abs(p - this._last) >= DRAG_THRESHOLD) {
            vibrate(twice(DRAG_TEXTURE));
            this._last = p;
        }
        this._atUpper = this._atLower = false;
    }

    // onHandleReleasedFromTouch
    released() {
        this._last = -1;
    }
}

function roundedRect(cr, x, y, w, h, rTop, rBottom) {
    rTop = Math.min(rTop, w / 2, h / 2);
    rBottom = Math.min(rBottom, w / 2, h / 2);
    cr.newPath();
    cr.arc(x + rTop, y + rTop, rTop, Math.PI, 1.5 * Math.PI);
    cr.arc(x + w - rTop, y + rTop, rTop, 1.5 * Math.PI, 2 * Math.PI);
    cr.arc(x + w - rBottom, y + h - rBottom, rBottom, 0, 0.5 * Math.PI);
    cr.arc(x + rBottom, y + h - rBottom, rBottom, 0.5 * Math.PI, Math.PI);
    cr.closePath();
}

function shapeStyle(color, radius, alpha = 1) {
    return `background-color: ${rgba(color.map(Math.round), alpha)}; border-radius: ${px(radius).toFixed(2)}px;${gild(0.5)}`;
}

// --- the slider: track, handle and stream icon, 0 at the bottom

const VolumeSlider = GObject.registerClass(
class VolumeSlider extends Canvas {
    constructor(dialog) {
        super({
            x: px(SLIDER_X), y: px(S), width: px(SLIDER_W), height: px(SLIDER_H),
            reactive: true,
            accessible_name: 'Media volume',
        });
        this._dialog = dialog;
        this.value = 0;                 // the fraction shown, 0..1
        this.muted = false;
        this._iconOnActive = false;
        this._activeIconAlpha = 0;
        this._inactiveIconAlpha = 1;

        // a press takes the value at once; past the ends the drag pulls the whole pill
        const pan = new Clutter.PanGesture({pan_axis: Clutter.PanAxis.Y, begin_threshold: 0, max_n_points: 1});
        pan.connect('recognize', g => dialog.dragSlider(this._fractionAt(g)));
        pan.connect('pan-update', g => dialog.dragSlider(this._fractionAt(g)));
        pan.connect('end', () => dialog.endSliderDrag());
        pan.connect('cancel', () => dialog.endSliderDrag());
        this.add_action(pan);
    }

    // the fraction under the finger, unclamped; measured on the screen, not on the slider the overscroll moves
    _fractionAt(gesture) {
        const y = gesture.get_centroid_abs().y * SCALE - S;
        return (TRACK_TOP + TRACK_LEN - y) / TRACK_LEN;
    }

    setValue(value) {
        this.value = value;
        this.queue_repaint();
    }

    // the icon sits at the top of the inactive track until it no longer fits there, then under the handle
    _setIconSide(onActive) {
        if (onActive === this._iconOnActive)
            return;
        this._iconOnActive = onActive;
        this._iconAnim?.stop();
        const a0 = this._activeIconAlpha, i0 = this._inactiveIconAlpha;
        const total = ICON_IN_DELAY_MS + ICON_IN_MS;
        this._iconAnim = animate(this, {
            from: 0, to: 1, duration: total, interpolator: x => x,
            onUpdate: (v, f, t) => {
                const ms = t * total;
                const appear = clamp((ms - ICON_IN_DELAY_MS) / ICON_IN_MS), disappear = clamp(ms / ICON_OUT_MS);
                this._activeIconAlpha = onActive ? lerp(a0, 1, appear) : lerp(a0, 0, disappear);
                this._inactiveIconAlpha = onActive ? lerp(i0, 0, disappear) : lerp(i0, 1, appear);
                this.queue_repaint();
            },
            onDone: () => {
                this._iconAnim = null;
            },
        });
    }

    draw(cr) {
        screenPixels(cr);
        const v = clamp(this.value);
        const x = TRACK_INSET_X, bottom = TRACK_TOP + TRACK_LEN;
        const c = bottom - v * TRACK_LEN;
        const inactiveEnd = c - HANDLE_H / 2 - HANDLE_GAP;
        const activeStart = c + HANDLE_H / 2 + HANDLE_GAP;

        if (inactiveEnd > TRACK_TOP) {
            roundedRect(cr, x, TRACK_TOP, TRACK_W, inactiveEnd - TRACK_TOP, TRACK_RADIUS, TRACK_INNER_RADIUS);
            setSource(cr, Color.surfaceContainerHighest);
            cr.fill();
        }
        if (bottom > activeStart) {
            roundedRect(cr, x, activeStart, TRACK_W, bottom - activeStart, TRACK_INNER_RADIUS, TRACK_RADIUS);
            setSource(cr, Color.primary);
            cr.fill();
        }
        roundedRect(cr, 0, c - HANDLE_H / 2, SLIDER_W, HANDLE_H, HANDLE_H / 2, HANDLE_H / 2);
        setSource(cr, Color.primary);
        cr.fill();

        const icon = this.muted || v === 0 ? 'ic_volume_media_mute' : 'ic_volume_media';
        this._setIconSide(v >= ICON_ON_ACTIVE_FROM);
        const ix = x + (TRACK_W - ICON) / 2, inset = (ICON_BOX - ICON) / 2;
        if (this._inactiveIconAlpha > 0) {
            paintIcon(cr, icon, ix, TRACK_TOP + inset, ICON, Color.primary, this._inactiveIconAlpha);
        }
        if (this._activeIconAlpha > 0) {
            paintIcon(cr, icon, ix, activeStart + inset, ICON, Color.surfaceContainerHighest,
                this._activeIconAlpha);
        }
    }

    destroy() {
        this._iconAnim?.stop();
        super.destroy();
    }
});

// --- the dialog

export const VolumeDialog = GObject.registerClass(
class VolumeDialog extends St.Widget {
    constructor() {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'gnomePhoneVolume',
            x: monitor.x, y: monitor.y,
            width: px(ROOT_W), height: px(SCREEN_H),
            visible: false,
        });

        // the pill: its body under the slider, and the ringer's background on top (the cap, or the open drawer)
        this._body = new St.Widget({
            x: px(PILL_X), y: px(BODY_TOP), width: px(PILL_W), height: px(PILL_BOTTOM - BODY_TOP),
            reactive: true,
            style: `background-color: ${rgba(Color.surface)}; ` +
                `border-radius: 0 0 ${px(PILL_RADIUS).toFixed(2)}px ${px(PILL_RADIUS).toFixed(2)}px;`,
        });
        this.add_child(this._body);
        this._ringerBg = new St.Widget({x: px(PILL_X), width: px(PILL_W), reactive: true});
        this.add_child(this._ringerBg);

        this._slider = new VolumeSlider(this);
        this.add_child(this._slider);
        this._haptics = new SliderHaptics();
        this._keyHaptics = false;       // a volume key's step is animating

        // the settings button: three primary dots, opens the sound settings
        this._settings = new St.Button({
            x: px(PILL_X + (PILL_W - BUTTON) / 2), y: px(SETTINGS_TOP),
            width: px(BUTTON), height: px(BUTTON),
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            accessible_name: 'Sound settings',
        });
        const dots = new Canvas({width: px(BUTTON), height: px(BUTTON)});
        dots.draw = cr => {
            screenPixels(cr);
            setSource(cr, Color.primary);
            for (const dx of [-DOT_PITCH, 0, DOT_PITCH]) {
                cr.arc(BUTTON / 2 + dx, BUTTON / 2, DOT_RADIUS, 0, 2 * Math.PI);
                cr.fill();
            }
        };
        const ripple = new St.Widget({
            x: px((BUTTON - SETTINGS_RIPPLE) / 2), y: px((BUTTON - SETTINGS_RIPPLE) / 2),
            width: px(SETTINGS_RIPPLE), height: px(SETTINGS_RIPPLE),
            style: shapeStyle(Color.onSurface, SETTINGS_RIPPLE / 2, 0.1),
            opacity: 0,
        });
        const box = new St.Widget({width: px(BUTTON), height: px(BUTTON)});
        box.add_child(ripple);
        box.add_child(dots);
        this._settings.set_child(box);
        this._settings.connect('notify::pressed', () => {
            ripple.opacity = this._settings.pressed ? 255 : 0;
        });
        this._settings.connect('clicked', () => this._openSettings());
        this.add_child(this._settings);

        this._setupRinger();

        // show/hide spring on f; the pill is opaque whenever any of it is on screen
        this._f = 0;
        this._showSpring = new Spring({stiffness: SHOW_STIFFNESS, dampingRatio: SHOW_DAMPING, threshold: 0.001});
        this._shown = false;
        this._overscroll = 0;
        this._dragging = false;
        this._suppressUntil = 0;

        // the default output
        this._control = Volume.getMixerControl();
        this._control.connectObject(
            'default-sink-changed', () => this._syncStream(),
            'state-changed', () => this._syncStream(),
            this);
        this._syncStream();

        this.chord = null;                  // chord.js's, which volume down goes through
        this._takeVolumeKeys();
        this.connect('destroy', () => this._onDestroy());
    }

    // --- the volume keys: taken from gnome-settings-daemon's accelerators in the shell's D-Bus service

    _takeVolumeKeys() {
        const dbus = Main.shellDBusService;
        if (!dbus?._emitAcceleratorActivated)
            return;
        this._dbus = dbus;
        this._volumeActions = new Map();       // accelerator action -> +1 / -1
        this._takenKeys = new Set();           // ... handed to the app that took the keys, or dropped, until let go
        const activated = dbus._emitAcceleratorActivated;
        const deactivated = dbus._emitAcceleratorDeactivated;
        this._restoreKeys = () => {
            dbus._emitAcceleratorActivated = activated;
            dbus._emitAcceleratorDeactivated = deactivated;
        };
        dbus._emitAcceleratorActivated = (action, device, timestamp) => {
            const direction = this._volumeKey(action);
            if (!direction) {
                activated.call(dbus, action, device, timestamp);
                return;
            }
            const event = Clutter.get_current_event();
            const repeat = !!(event && event.get_flags() & Clutter.EventFlags.FLAG_REPEATED);
            // The app in front took the volume keys (service.js TakeVolumeKeys): it gets this one, and the release
            // is swallowed. On the press, where Android acts on the release: the shell is not always told of the
            // release (none came for a key from a virtual keyboard, 2026-11-07), and a ringing timer that a press
            // could not stop would go on ringing.
            const taker = this.keysTaker?.();
            if (taker) {
                this._takenKeys.add(action);
                if (!repeat)
                    taker(direction);
                return;
            }
            // the screen off, the AOD or black: the keys do nothing, neither the volume nor the screen changes
            if (this.screenOff?.()) {
                this._takenKeys.add(action);
                return;
            }
            // volume down waits for the power key: the two are a screenshot (chord.js)
            if (direction < 0 && this.chord)
                this.chord.volumeDown(() => this.volumeKey(direction, repeat), repeat);
            else
                this.volumeKey(direction, repeat);
        };
        dbus._emitAcceleratorDeactivated = (action, device, timestamp) => {
            if (this._takenKeys.delete(action))
                return;
            if (!this._volumeActions.has(action))
                deactivated.call(dbus, action, device, timestamp);
            else if (this._volumeActions.get(action) < 0)
                this.chord?.volumeUp();
        };
    }

    // which grabbed accelerator is a volume key: learnt from the key event that fires it
    _volumeKey(action) {
        if (this._volumeActions.has(action))
            return this._volumeActions.get(action);
        const event = Clutter.get_current_event();
        if (event?.type() !== Clutter.EventType.KEY_PRESS)
            return 0;
        const symbol = event.get_key_symbol();
        const direction = symbol === Clutter.KEY_AudioRaiseVolume ? 1
            : symbol === Clutter.KEY_AudioLowerVolume ? -1 : 0;
        if (direction)
            this._volumeActions.set(action, direction);
        return direction;
    }

    volumeKey(direction, repeat = false) {
        const now = GLib.get_monotonic_time() / 1000;
        if (!this._shown) {
            this.show();
            // nothing playing: the press only shows the panel, and so does that key held for 500 ms
            // (AudioService.adjustSuggestedStreamVolume)
            if (!this._playing()) {
                this._suppressUntil = now + SUPPRESS_MS;
                return;
            }
        }
        this._restartTimeout();
        if (repeat && now < this._suppressUntil)
            return;
        this._keyHaptics = true;
        this._setLevel(clamp((this._level ?? 0) + direction, 0, LEVELS), true);
    }

    _playing() {
        return this._control.get_sink_inputs().some(s =>
            !s.is_event_stream && s.state === Gvc.MixerStreamState.RUNNING);
    }

    // --- the stream

    _syncStream() {
        const stream = this._control.get_state() === Gvc.MixerControlState.READY
            ? this._control.get_default_sink() : null;
        if (stream === this._stream)
            return;
        this._stream?.disconnectObject(this);
        this._stream = stream;
        stream?.connectObject(
            'notify::volume', () => this._streamChanged(),
            'notify::is-muted', () => this._streamChanged(),
            this);
        this._streamChanged();
    }

    _streamLevel() {
        const stream = this._stream;
        if (!stream || stream.is_muted)
            return 0;
        return clamp(Math.round(stream.volume / this._control.get_vol_max_norm() * LEVELS), 0, LEVELS);
    }

    _streamChanged() {
        const level = this._streamLevel();
        this._slider.muted = !!this._stream?.is_muted;
        if (level === this._level)
            return;
        this._level = level;
        // after a drag, what the stream reports is not shown for a while (it lags the finger)
        if (this._dragging || GLib.get_monotonic_time() / 1000 < (this._ignoreUntil ?? 0))
            return;
        this._showValue(level / LEVELS, this._shown);
    }

    _setLevel(level, animated) {
        this._level = level;
        const stream = this._stream;
        if (stream) {
            if (level > 0 && stream.is_muted)
                stream.change_is_muted(false);
            stream.volume = Math.round(this._control.get_vol_max_norm() * level / LEVELS);
            stream.push_volume();
        }
        this._showValue(level / LEVELS, animated);
    }

    // the slider's value: Animatable.animateTo on spring(1, 1500), velocity kept across presses
    _showValue(value, animated) {
        if (!animated) {
            this._valueAnim?.stop();
            this._valueAnim = null;
            this._valueSpring = null;
            this._keyHaptics = false;
            this._slider.setValue(value);
            return;
        }
        if (this._valueSpring) {
            this._valueSpring.target = value;
            return;
        }
        this._valueSpring = new Spring({
            stiffness: VALUE_STIFFNESS, dampingRatio: 1, value: this._slider.value, target: value,
            threshold: 0.01 / LEVELS,
        });
        this._valueAnim = animateSpring(this, this._valueSpring, v => {
            this._slider.setValue(v);
            // a key's step is felt along the way, as the tracker follows the animated progress
            if (this._keyHaptics)
                this._haptics.progress(clamp(v));
        }, () => {
            this._valueAnim = null;
            this._valueSpring = null;
            this._keyHaptics = false;
        });
    }

    // --- dragging the slider: discrete levels, the pill pulled along past the ends

    dragSlider(fraction) {
        if (!this._dragging) {
            this._dragging = true;
            this._overscrollAnim?.stop();
            this._overscrollAnim = null;
        }
        this._restartTimeout();
        this._haptics.progress(clamp(fraction));
        const level = Math.round(clamp(fraction) * LEVELS);
        if (level !== this._level)
            this._setLevel(level, false);
        else
            this._showValue(level / LEVELS, false);
        // VolumeDialogOverscrollViewModel: a third of the way past the end, eased onto at most 56 dp
        const past = fraction > 1 ? (fraction - 1) * TRACK_LEN : fraction < 0 ? fraction * TRACK_LEN : 0;
        const amount = OVERSCROLL(clamp(Math.abs(past) / 3 / OVERSCROLL_MAX)) * OVERSCROLL_MAX;
        this._setOverscroll(-Math.sign(past) * amount);
    }

    endSliderDrag() {
        if (!this._dragging)
            return;
        this._dragging = false;
        this._haptics.released();
        this._ignoreUntil = GLib.get_monotonic_time() / 1000 + 1000;
        this._restartTimeout();
        if (this._overscroll === 0)
            return;
        const spring = new Spring({
            stiffness: OVERSCROLL_STIFFNESS, dampingRatio: OVERSCROLL_DAMPING, value: this._overscroll, target: 0,
            threshold: 0.5,
        });
        this._overscrollAnim = animateSpring(this, spring, v => this._setOverscroll(v), () => {
            this._overscrollAnim = null;
        });
    }

    _setOverscroll(y) {
        this._overscroll = y;
        this.translation_y = px(y);
    }

    // --- the ringer

    _setupRinger() {
        const source = Gio.SettingsSchemaSource.get_default();
        this._feedback = source.lookup(FEEDBACKD_SCHEMA, true) ? new Gio.Settings({schema_id: FEEDBACKD_SCHEMA})
            : null;
        this._drawer = 0;               // 0 closed .. 1 open
        this._ringerButtons = RINGER_MODES.map((mode, i) => {
            const button = new St.Button({
                width: px(RINGER_SLOT), height: px(RINGER_SLOT),
                x: px(PILL_X + (PILL_W - RINGER_SLOT) / 2),
                can_focus: true,
                button_mask: St.ButtonMask.PRIMARY,
                accessible_name: mode.name,
            });
            const box = new St.Widget({width: px(RINGER_SLOT), height: px(RINGER_SLOT)});
            const circle = new St.Widget({
                x: px((RINGER_SLOT - RINGER_BUTTON) / 2), y: px((RINGER_SLOT - RINGER_BUTTON) / 2),
                width: px(RINGER_BUTTON), height: px(RINGER_BUTTON),
            });
            const icon = new VectorIcon(mode.icon, px(RINGER_ICON), Color.onPrimary);
            icon.set_position(px((RINGER_BUTTON - RINGER_ICON) / 2), px((RINGER_BUTTON - RINGER_ICON) / 2));
            circle.add_child(icon);
            box.add_child(circle);
            button.set_child(box);
            button.connect('clicked', () => this._ringerClicked(i));
            this.add_child(button);
            return {button, circle, icon, selected: 0};
        });
        this._ringer = this._ringerIndex();
        this._ringerButtons.forEach((b, i) => {
            b.selected = i === this._ringer ? 1 : 0;
        });
        this._feedback?.connectObject('changed::profile', () => {
            const index = this._ringerIndex();
            if (index !== this._ringer)
                this._selectRinger(index);
        }, this);
        this._layoutRinger();
    }

    _ringerIndex() {
        const profile = this._feedback?.get_string('profile') ?? 'full';
        return Math.max(RINGER_MODES.findIndex(m => m.profile === profile), 0);
    }

    _ringerClicked(index) {
        this._restartTimeout();
        if (this._drawer < 1 && !this._drawerOpen) {
            if (index === this._ringer)
                this._animateDrawer(true);
            return;
        }
        if (index !== this._ringer) {
            this._feedback?.set_string('profile', RINGER_MODES[index].profile);
            this._selectRinger(index);
        }
        // the drawer closes 300 ms after a choice
        if (this._closeDrawerId)
            return;
        this._closeDrawerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RINGER_CLOSE_DELAY_MS, () => {
            this._closeDrawerId = 0;
            this._animateDrawer(false);
            return GLib.SOURCE_REMOVE;
        });
    }

    // colours on a spring k 3800, the corner on k 800 / 0.6 (VolumeDialogRingerViewBinder)
    _selectRinger(index) {
        this._ringer = index;
        this._ringerButtons.forEach((b, i) => {
            const target = i === index ? 1 : 0;
            b.colorAnim?.stop();
            b.shapeAnim?.stop();
            b.colorT ??= b.selected;
            b.shapeT ??= b.selected;
            b.selected = target;
            if (!this._shown) {
                b.colorT = b.shapeT = target;
                return;
            }
            b.colorAnim = animateSpring(this, new Spring({stiffness: 3800, dampingRatio: 1, value: b.colorT,
                target}), v => {
                b.colorT = v;
                this._styleRingerButton(b);
            }, () => {
                b.colorAnim = null;
            });
            b.shapeAnim = animateSpring(this, new Spring({stiffness: 800, dampingRatio: 0.6, value: b.shapeT,
                target}), v => {
                b.shapeT = v;
                this._styleRingerButton(b);
            }, () => {
                b.shapeAnim = null;
            });
        });
        this._layoutRinger();
    }

    _styleRingerButton(b) {
        const c = clamp(b.colorT ?? b.selected), s = b.shapeT ?? b.selected;
        const bg = Color.surfaceContainerHighest.map((v, i) => lerp(v, Color.primary[i], c));
        const fg = Color.onSurface.map((v, i) => lerp(v, Color.onPrimary[i], c));
        b.circle.style = shapeStyle(bg, Math.max(lerp(UNSELECTED_RADIUS, SELECTED_RADIUS, s), 0));
        b.icon.setColor(fg);
    }

    _animateDrawer(open) {
        if (this._closeDrawerId) {
            GLib.source_remove(this._closeDrawerId);
            this._closeDrawerId = 0;
        }
        this._drawerOpen = open;
        this._drawerAnim?.stop();
        const from = this._drawer, to = open ? 1 : 0;
        this._drawerAnim = animate(this, {
            from, to, duration: RINGER_OPEN_MS * Math.abs(to - from),
            interpolator: open ? EMPHASIZED_DECELERATE : RINGER_CLOSE,
            onUpdate: v => {
                this._drawer = v;
                this._layoutRinger();
            },
            onDone: () => {
                this._drawerAnim = null;
            },
        });
    }

    // closed, the selected button sits in the cap and the others fold under it at 1 dp each; open, they stack
    _layoutRinger() {
        const d = this._drawer;
        const top = lerp(RINGER_BG_CLOSED, RINGER_BG_OPEN, d);
        this._ringerBg.set_position(px(PILL_X), px(top));
        this._ringerBg.set_size(px(PILL_W), px(RINGER_BG_BOTTOM - top));
        this._ringerBg.style = shapeStyle(Color.surface, lerp(PILL_RADIUS, RINGER_OPEN_RADIUS, d));
        this._ringerButtons.forEach((b, i) => {
            const selected = i === this._ringer;
            const closed = RINGER_CENTER - RINGER_HIDDEN_STEP * (selected ? this._ringer : i);
            const center = lerp(closed, RINGER_CENTER - RINGER_SLOT * i, d);
            b.button.y = px(center - RINGER_SLOT / 2);
            b.button.opacity = selected ? 255 : Math.round(255 * d);
            b.button.visible = selected || d > 0;
            this._styleRingerButton(b);
        });
    }

    // --- showing and hiding

    show() {
        raiseToTop(this);
        this._shown = true;
        this._syncStreamValue();
        this._animateShow(1);
        this._restartTimeout();
        this._watchOutside(true);
    }

    hide() {
        if (!this._shown)
            return;
        this._shown = false;
        this._clearTimeout();
        this._watchOutside(false);
        if (this._drawer > 0)
            this._animateDrawer(false);
        this._animateShow(0);
    }

    _syncStreamValue() {
        this._level = this._streamLevel();
        this._slider.muted = !!this._stream?.is_muted;
        if (this._f === 0)
            this._showValue(this._level / LEVELS, false);
    }

    // one spring both ways: a show during a hide turns it round with its velocity
    _animateShow(target) {
        this._showSpring.target = target;
        this.visible = true;
        if (this._showAnim)
            return;
        this._showAnim = animateSpring(this, this._showSpring, f => this._setFraction(f), () => {
            this._showAnim = null;
            if (this._showSpring.target === 0)
                this._setFraction(0);
        });
    }

    _setFraction(f) {
        this._f = f;
        this.translation_x = px(lerp(-ROOT_W, 0, f));
        this.visible = f > 0 || this._shown;
    }

    _restartTimeout() {
        this._clearTimeout();
        this._timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, DISMISS_MS, () => {
            this._timeoutId = 0;
            if (this._dragging) {
                this._restartTimeout();
                return GLib.SOURCE_REMOVE;
            }
            this.hide();
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearTimeout() {
        if (this._timeoutId)
            GLib.source_remove(this._timeoutId);
        this._timeoutId = 0;
    }

    // a touch outside the pill dismisses it and still goes where it was going. Over a modal (the open shade) nothing
    // else gets a touch, so there the panel takes its own grab on top, as GNOME's popups do: the touch that
    // dismisses it goes no further, and Escape dismisses it as Back would
    _watchOutside(watch) {
        if (!watch) {
            if (this._capturedId)
                this._watched.disconnect(this._capturedId);
            this._capturedId = 0;
            this._watched = null;
            if (this._grabbed)
                Main.popModal(this._grabbed);
            this._grabbed = null;
            return;
        }
        if (this._capturedId)
            return;
        if (Main.modalCount > 0)
            this._grabbed = Main.pushModal(this, {actionMode: Shell.ActionMode.POPUP});
        this._watched = this._grabbed ? this : global.stage;
        this._capturedId = this._watched.connect('captured-event', (_actor, event) => {
            const type = event.type();
            if (type === Clutter.EventType.KEY_PRESS && this._grabbed &&
                event.get_key_symbol() === Clutter.KEY_Escape) {
                this.hide();
                return Clutter.EVENT_STOP;
            }
            if (type !== Clutter.EventType.TOUCH_BEGIN && type !== Clutter.EventType.BUTTON_PRESS)
                return Clutter.EVENT_PROPAGATE;
            const [x, y] = event.get_coords();
            const target = global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE, x, y);
            if (!target || !this.contains(target))
                this.hide();
            return Clutter.EVENT_PROPAGATE;
        });
    }

    // the grab gives the panel key focus, and :focus would restyle all inside it (Shade.vfunc_key_focus_in)
    vfunc_key_focus_in() {
    }

    vfunc_key_focus_out() {
    }

    _openSettings() {
        this.hide();
        openSettings('sound');
    }

    _onDestroy() {
        this._restoreKeys?.();
        this._clearTimeout();
        this._watchOutside(false);
        if (this._closeDrawerId)
            GLib.source_remove(this._closeDrawerId);
        for (const anim of [this._showAnim, this._valueAnim, this._overscrollAnim, this._drawerAnim])
            anim?.stop();
        for (const b of this._ringerButtons) {
            b.colorAnim?.stop();
            b.shapeAnim?.stop();
        }
    }
});
