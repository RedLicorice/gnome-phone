// Quick Settings as Android 16 draws it in the shade: the header (clock, date, carrier, system icons), the tile grid
// whose first two rows are the QQS, the brightness slider, the build row and the footer. The shade lays it out once
// per frame from its expansion f and the QS expansion q (spec §2.1, §2.6, §3); tiles act on SystemState.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as SystemActions from 'resource:///org/gnome/shell/misc/systemActions.js';

import {addEndMark} from './endmark.js';
import {Canvas, hasIcon, paintIcon, VectorIcon} from './icons.js';
import {animate, animateSpring, clamp, lerp, Spring} from './motion.js';
import {Color, cogl, font, gild, mix, rgba, setSource, SurfaceEffect} from './palette.js';
import {openSettings} from './settings.js';
import {inkBox, SystemIcons} from './statusbar.js';
import {px, screenPixels, SCALE, SCREEN_H, SCREEN_W} from './units.js';

// --- geometry, screen px (spec §3)
export const QQS_HEIGHT = 644;          // the stack's top padding: 116 + 2 x 216 + 24 + 72
export const QS_SHIFT = 504;            // the QQS rows move 116 -> 620 as q goes 0 -> 1
const GRID_X = 48;
const GRID_TOP = 116;
const TILE_W = 228;                     // 76 dp
const TILE_H = 216;                     // 72 dp
const COL_PITCH = 252;
const ROW_GAP = 24;
const COLUMNS = 4;
const QQS_ROWS = 2;
const ICON = 96;                        // 32 dp
const LARGE_ICON = 84;                  // 28 dp, centred in a 56 dp target after 8 dp padding
const LARGE_ICON_CX = 108;
const LARGE_TEXT_X = 210;               // 8 + 56 + 6 dp
const TILE_RADIUS = 72;                 // 24 dp, on or off: Android's off tile is a pill, this phone's a squircle too
const CONTENT_FADE_FROM = 0.43;         // QS content fades in over q 0.43..1, the footer over 0.9..1
const FOOTER_FADE_FROM = 0.9;

const SLIDER_X = 54;
const SLIDER_END = 867;                 // then a 10 dp spacer and the 45 dp auto button
const SLIDER_CY = 500;                  // QS position
const TRACK_H = 120;                    // 40 dp
const TRACK_RADIUS = 36;                // 12 dp, 2 dp next to the thumb
const TRACK_INNER_RADIUS = 6;
const THUMB_W = 12;                     // 4 x 52 dp
const THUMB_H = 156;
const THUMB_GAP = 18;                   // 6 dp each side
const TRACK_ICON = 84;                  // 28 dp, 6 dp from the segment's end
const TRACK_ICON_PAD = 18;
const AUTO_X = 897;
const AUTO_SIZE = 135;                  // 45 dp

const BUILD_INK_X = 49;
const BUILD_INK_TOP = 1612;
const FOOTER_CY = 2195.5;
const FOOTER_BUTTON = 120;              // 40 dp circles
const POWER_X = 912;
const SETTINGS_X = 768;
const FOOTER_ICON = 60;                 // 20 dp

// header: ink positions of the QQS row (q = 0) and the QS layout (q = 1)
const CLOCK_QQS = [50, 24];
const CLOCK_QS = [53, 80];
const CLOCK_SCALE = 2.57;
const DATE_QQS = [198, 24];
const DATE_QS = [49, 189];
const ICONS_QQS = [912, 42];            // the cluster's right end and centre line
const ICONS_QS = [1031, 204];
const CARRIER_END = 1019;
const CARRIER_QQS_TOP = 24;
const CARRIER_QS_TOP = 110;
const HEADER_FADE_OUT = 0.14;           // combined_qs_header_scene.xml keyframes
const HEADER_FADE_IN = 0.58;
const HEADER_DISPLACEMENT = 0.1066;

// fonts fitted to the stills' pixels (Google Sans Flex, rounded)
const TILE_LABEL_FONT = font(14, 600);              // variable-title-small-emphasized
const TILE_SECONDARY_FONT = font(12, 500);          // label-medium
const CLOCK_FONT = font(14, 650);                   // variable-body-medium-emphasized
const CLOCK_BIG_FONT = font(14 * CLOCK_SCALE, 650, {opsz: 14});
const DATE_FONT = font(14, 600);
const CARRIER_FONT = font(14, 550);
const BUILD_FONT = font(12, 500);

const CAP_HEIGHT = 0.73;                // cap height / em of the font (spec §4: 0.72-0.74)

// Tile colours (Tile.kt TileDefaults): [background rgb, alpha], foreground [rgb, alpha]
const TILE_COLORS = {
    active: {bg: [Color.primary, 1], fg: [Color.onPrimary, 1]},
    inactive: {bg: SurfaceEffect[1], fg: [Color.onSurface, 1]},
    unavailable: {bg: [Color.surface, 0.18], fg: [Color.onSurfaceVariant, 0.38]},
};

const MOBILE_ICONS = ['ic_signal_cellular_0_4_bar', 'ic_signal_cellular_1_4_bar', 'ic_signal_cellular_2_4_bar',
    'ic_signal_cellular_3_4_bar', 'ic_signal_cellular_4_4_bar'];


// The tiles of this phone, in AOSP's default order (no power-profiles-daemon: battery saver shows unavailable)
// state: 'active' | 'inactive' | 'unavailable'
const TILES = [
    {
        id: 'wifi', label: 'Wi-Fi', panel: 'wifi',
        icon: s => !s.wifi.enabled ? 'ic_signal_wifi_off'
            : s.wifi.connected ? `ic_wifi_${[0, 1, 2, 2, 3][s.wifi.level] ?? 0}` : 'ic_wifi_0',
        state: s => !s.wifi.available ? 'unavailable' : s.wifi.enabled ? 'active' : 'inactive',
        toggle: s => s.setWifiEnabled(!s.wifi.enabled),
    },
    {
        id: 'bluetooth', label: 'Bluetooth', panel: 'bluetooth',
        icon: s => !s.bluetooth.powered ? 'qs_bluetooth_icon_off'
            : s.bluetooth.connected ? 'qs_bluetooth_icon_on' : 'qs_bluetooth_icon_disconnected',
        state: s => !s.bluetooth.available ? 'unavailable' : s.bluetooth.powered ? 'active' : 'inactive',
        toggle: s => s.setBluetooth(!s.bluetooth.powered),
    },
    {
        id: 'flashlight', label: 'Flashlight',
        icon: s => s.torch ? 'qs_flashlight_icon_on' : 'qs_flashlight_icon_off',
        state: s => s.torch === null ? 'unavailable' : s.torch ? 'active' : 'inactive',
        toggle: s => s.setTorch(!s.torch),
    },
    {
        id: 'dnd', label: 'Do Not Disturb', panel: 'notifications',
        icon: () => 'ic_qs_dnd_on',                 // the filled glyph in both states, as on the device
        state: s => s.dnd ? 'active' : 'inactive',
        toggle: s => {
            s.dnd = !s.dnd;
        },
    },
    {
        id: 'airplane', label: 'Airplane mode', panel: 'network',
        icon: s => s.airplane ? 'qs_airplane_icon_on' : 'qs_airplane_icon_off',
        state: s => s.airplane ? 'active' : 'inactive',
        toggle: s => s.setAirplane(!s.airplane),
    },
    {
        id: 'mobile', label: 'Mobile data', panel: 'wwan', wide: true,
        icon: s => !s.mobile.present ? 'ic_qs_no_sim'
            : s.mobile.dataEnabled ? MOBILE_ICONS[s.mobile.level] ?? MOBILE_ICONS[0] : 'ic_signal_mobile_data_off',
        state: s => !s.mobile.present || s.airplane ? 'unavailable' : s.mobile.dataEnabled ? 'active' : 'inactive',
        secondary: s => !s.mobile.present ? 'No SIM' : s.mobile.dataEnabled ? s.mobile.operator || 'On' : 'Off',
        toggle: s => s.setMobileData(!s.mobile.dataEnabled),
    },
    {
        id: 'rotation', label: 'Auto-rotate', panel: 'display',
        icon: s => s.autoRotate ? 'qs_auto_rotate_icon_on' : 'qs_auto_rotate_icon_off',
        state: s => s.autoRotate ? 'active' : 'inactive',
        toggle: s => {
            s.autoRotate = !s.autoRotate;
        },
    },
    {
        id: 'location', label: 'Location', panel: 'location',
        icon: s => s.location ? 'qs_location_icon_on' : 'qs_location_icon_off',
        state: s => s.location ? 'active' : 'inactive',
        toggle: s => {
            s.location = !s.location;
        },
    },
    {
        id: 'dark', label: 'Dark theme', panel: 'appearance',
        icon: s => s.darkMode ? 'qs_light_dark_theme_icon_on' : 'qs_light_dark_theme_icon_off',
        state: s => s.darkMode ? 'active' : 'inactive',
        toggle: s => {
            s.darkMode = !s.darkMode;
        },
    },
    {
        id: 'saver', label: 'Battery Saver', panel: 'power',
        icon: s => s.powerSave ? 'qs_battery_saver_icon_on' : 'qs_battery_saver_icon_off',
        state: s => s.powerSave === null ? 'unavailable' : s.powerSave ? 'active' : 'inactive',
        toggle: s => s.setPowerSave(!s.powerSave),
    },
    {
        id: 'nightlight', label: 'Night Light', panel: 'display',
        icon: s => s.nightLight ? 'qs_nightlight_icon_on' : 'qs_nightlight_icon_off',
        state: s => s.nightLight ? 'active' : 'inactive',
        toggle: s => {
            s.nightLight = !s.nightLight;
        },
    },
];

function colorStyle([c, a]) {
    return rgba(c.map(Math.round), a);
}

// interpolate two [rgb, alpha] colours
function mixColor([ca, aa], [cb, ab], t) {
    return [mix(ca, cb, t), lerp(aa, ab, t)];
}

// Position a Clutter.Text so its first line's cap height is centred on @capCenterY, text box start at @x (logical)
function placeCapCentered(text, x, capCenterY, sizePx) {
    const baseline = text.get_layout().get_baseline() / 1024 / text.get_resource_scale();
    text.set_position(x, capCenterY + CAP_HEIGHT * sizePx / 2 - baseline);
}

// --- tiles

const Tile = GObject.registerClass(
class Tile extends St.Button {
    constructor(spec, state, onActivated) {
        super({
            reactive: true,
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            width: px(spec.wide ? 2 * TILE_W + ROW_GAP : TILE_W),
            height: px(TILE_H),
        });
        this._spec = spec;
        this._state = state;
        this.wide = !!spec.wide;
        this.accessible_name = spec.label;

        this._content = new St.Widget({x_expand: true, y_expand: true});
        this.set_child(this._content);

        this._icon = new VectorIcon(spec.icon(state), px(this.wide ? LARGE_ICON : ICON), Color.onSurface);
        this._content.add_child(this._icon);
        if (this.wide) {
            this._label = new Clutter.Text({font_name: TILE_LABEL_FONT, text: spec.label});
            this._secondary = new Clutter.Text({font_name: TILE_SECONDARY_FONT});
            this._content.add_child(this._label);
            this._content.add_child(this._secondary);
        }

        // the press highlight: onSurface at 15 %
        this._highlight = new St.Widget({
            style: `background-color: ${rgba(Color.onSurface, 0.15)};`,
            opacity: 0,
        });
        this._content.add_child(this._highlight);
        this.connect('notify::pressed', () => {
            this._highlight.opacity = this.pressed ? 255 : 0;
        });

        this.connect('clicked', () => {
            spec.toggle(state);
            onActivated?.(this);
        });
        if (spec.panel && Clutter.LongPressGesture) {
            const longPress = new Clutter.LongPressGesture({required_button: Clutter.BUTTON_PRIMARY});
            longPress.connect('recognize', () => {
                openSettings(spec.panel);
                onActivated?.(this, true);
            });
            this.add_action(longPress);
        }

        // the colours spring between inactive and active (Tile.kt:586-600); the corner stays
        this._mode = spec.state(state);
        this._from = TILE_COLORS[this._mode];
        this._to = TILE_COLORS[this._mode];
        this._colorT = 1;
        state.connectObject('changed', () => this._sync(), this);
        this._heightPx = TILE_H;
        this._applyStyle();
        this._sync();
    }

    _sync() {
        const spec = this._spec, state = this._state;
        this._icon.name = spec.icon(state);
        if (this.wide)
            this._secondary.text = spec.secondary?.(state) ?? '';

        const mode = spec.state(state);
        this.reactive = mode !== 'unavailable' || !!spec.panel;
        if (mode === this._mode) {
            this._layoutContent();
            return;
        }
        // restart the colour spring from where it is now
        const current = this._colors();
        this._from = current;
        this._to = TILE_COLORS[mode];
        this._mode = mode;
        this._colorT = 0;
        this._spring?.stop();
        const spring = new Spring({stiffness: 1500, dampingRatio: 1, value: 0, target: 1});
        this._spring = animateSpring(this, spring, v => {
            this._colorT = v;
            this._applyStyle();
        }, () => {
            this._spring = null;
        });
        this._applyStyle();
        this._layoutContent();
    }

    _colors() {
        const t = clamp(this._colorT, 0, 1);
        return {bg: mixColor(this._from.bg, this._to.bg, t), fg: mixColor(this._from.fg, this._to.fg, t)};
    }

    _applyStyle() {
        const {bg, fg} = this._colors();
        this.style = `background-color: ${colorStyle(bg)}; border-radius: ${px(TILE_RADIUS).toFixed(2)}px;${gild()}`;
        this._highlight.style = `background-color: ${rgba(Color.onSurface, 0.15)}; ` +
            `border-radius: ${px(TILE_RADIUS).toFixed(2)}px;`;
        this._icon.setColor(fg[0], fg[1]);
        if (this.wide) {
            this._label.color = cogl(fg[0], fg[1]);
            this._secondary.color = cogl(fg[0], fg[1]);
        }
    }

    // the squished height (screen px); the content stays centred and unscaled (SquishTile.kt)
    setHeightPx(h) {
        if (Math.abs(h - this._heightPx) < 0.01)
            return;
        this._heightPx = h;
        this.height = px(h);
        this._layoutContent();
    }

    _layoutContent() {
        const h = this._heightPx;
        const w = this.wide ? 2 * TILE_W + ROW_GAP : TILE_W;
        this._highlight.set_size(px(w), px(h));
        const cy = h / 2;
        if (!this.wide) {
            this._icon.set_position(px((w - ICON) / 2), px(cy - ICON / 2));
            return;
        }
        this._icon.set_position(px(LARGE_ICON_CX - LARGE_ICON / 2), px(cy - LARGE_ICON / 2));
        const labelSize = 14 * 3, secondarySize = 12 * 3;
        if (this._secondary.text) {
            // 20 sp + 16 sp lines, centred together
            placeCapCentered(this._label, px(LARGE_TEXT_X), px(cy - 24), px(labelSize));
            placeCapCentered(this._secondary, px(LARGE_TEXT_X), px(cy + 30), px(secondarySize));
        } else {
            placeCapCentered(this._label, px(LARGE_TEXT_X), px(cy), px(labelSize));
        }
        this._secondary.visible = !!this._secondary.text;
    }

    destroy() {
        this._spring?.stop();
        super.destroy();
    }
});

// --- brightness

const BrightnessSlider = GObject.registerClass(
class BrightnessSlider extends St.Widget {
    constructor(state, shade) {
        super({width: px(SCREEN_W), height: px(THUMB_H), reactive: false});
        this._state = state;
        this._shade = shade;

        this._track = new Canvas({
            x: px(SLIDER_X), y: 0, width: px(SLIDER_END - SLIDER_X), height: px(THUMB_H), reactive: true,
        });
        this._track.draw = cr => this._paintTrack(cr);
        this.add_child(this._track);

        this._value = state.brightness ?? 0.5;
        this._dragging = false;

        // press or drag anywhere on the track: with no threshold it takes the touch at once, so the shade's own
        // gestures give way (the Compose slider jumps to the press too)
        const pan = new Clutter.PanGesture({begin_threshold: 0, max_n_points: 1});
        pan.connect('recognize', g => this._dragTo(g));
        pan.connect('pan-update', g => this._dragTo(g));
        pan.connect('end', () => this._endDrag());
        pan.connect('cancel', () => this._endDrag());
        this._track.add_action(pan);

        // the auto-brightness button (a LineageOS addition): 45 dp, 22.5 dp corners off, 12 dp on
        this._auto = new St.Button({
            x: px(AUTO_X), y: px((THUMB_H - AUTO_SIZE) / 2), width: px(AUTO_SIZE), height: px(AUTO_SIZE),
            reactive: true, can_focus: true, accessible_name: 'Adaptive brightness',
        });
        this._autoIcon = new VectorIcon('ic_qs_brightness_auto_off', px(72), Color.onSurface);
        this._auto.set_child(this._autoIcon);
        this._auto.connect('clicked', () => {
            state.autoBrightness = !state.autoBrightness;
        });
        this.add_child(this._auto);
        this._autoT = state.autoBrightness ? 1 : 0;
        this._autoOn = state.autoBrightness;
        this._styleAuto();

        // the track icon cross-fades between the two segments (appear 100 ms after 33, disappear 50 ms)
        this._iconOnActive = false;
        this._activeIconAlpha = 0;
        this._inactiveIconAlpha = 1;

        state.connectObject('changed', () => this._sync(), this);
    }

    get dragging() {
        return this._dragging;
    }

    _sync() {
        if (!this._dragging) {
            const v = this._state.brightness;
            if (v !== null && Math.abs(v - this._value) > 1e-4) {
                this._value = v;
                this._track.queue_repaint();
            }
        }
        if (this._state.autoBrightness !== this._autoOn) {
            this._autoOn = this._state.autoBrightness;
            this._autoSpring?.stop();
            const from = this._autoT;
            const spring = new Spring({stiffness: 1500, dampingRatio: 1, value: from, target: this._autoOn ? 1 : 0});
            this._autoSpring = animateSpring(this, spring, v => {
                this._autoT = v;
                this._styleAuto();
            }, () => {
                this._autoSpring = null;
            });
        }
    }

    _styleAuto() {
        const t = clamp(this._autoT, 0, 1);
        const bg = mixColor(SurfaceEffect[1], [Color.primary, 1], t);
        const fg = mix(Color.onSurface, Color.onPrimary, t);
        const radius = lerp(67.5, TRACK_RADIUS, t);
        this._auto.style = `background-color: ${colorStyle(bg)}; border-radius: ${px(radius).toFixed(2)}px;${gild()}`;
        this._autoIcon.setColor(fg);
        this._autoIcon.name = this._autoOn ? 'ic_qs_brightness_auto_on' : 'ic_qs_brightness_auto_off';
    }

    _dragTo(gesture) {
        const point = gesture.get_point_coords(0);      // in the track's own coordinates
        const w = SLIDER_END - SLIDER_X;
        const v = clamp((point.x * SCALE - THUMB_W / 2) / (w - THUMB_W));
        if (!this._dragging) {
            this._dragging = true;
            this._shade.setBrightnessDragging?.(true);
        }
        this._value = v;
        this._state.brightness = v;
        this._track.queue_repaint();
    }

    _endDrag() {
        if (!this._dragging)
            return;
        this._dragging = false;
        this._shade.setBrightnessDragging?.(false);
    }

    _setIconSide(onActive) {
        if (onActive === this._iconOnActive)
            return;
        this._iconOnActive = onActive;
        this._iconAnim?.stop();
        const a0 = this._activeIconAlpha, i0 = this._inactiveIconAlpha;
        // appear: 100 ms after 33 ms; disappear: 50 ms
        this._iconAnim = animate(this, {
            from: 0, to: 1, duration: 133, interpolator: x => x,
            onUpdate: (v, f, t) => {
                const ms = t * 133;
                const appear = clamp((ms - 33) / 100), disappear = clamp(ms / 50);
                this._activeIconAlpha = onActive ? lerp(a0, 1, appear) : lerp(a0, 0, disappear);
                this._inactiveIconAlpha = onActive ? lerp(i0, 0, disappear) : lerp(i0, 1, appear);
                this._track.queue_repaint();
            },
            onDone: () => {
                this._iconAnim = null;
            },
        });
    }

    _paintTrack(cr) {
        screenPixels(cr);
        const w = SLIDER_END - SLIDER_X;
        const c = THUMB_W / 2 + this._value * (w - THUMB_W);
        const top = (THUMB_H - TRACK_H) / 2;
        const activeEnd = c - THUMB_W / 2 - THUMB_GAP;
        const inactiveStart = c + THUMB_W / 2 + THUMB_GAP;

        const segment = (x0, x1, r0, r1, color, alpha) => {
            if (x1 - x0 <= 0)
                return;
            const h = TRACK_H;
            const left = Math.min(r0, (x1 - x0) / 2), right = Math.min(r1, (x1 - x0) / 2);
            cr.newPath();
            cr.arc(x0 + left, top + left, left, Math.PI, 1.5 * Math.PI);
            cr.arc(x1 - right, top + right, right, 1.5 * Math.PI, 2 * Math.PI);
            cr.arc(x1 - right, top + h - right, right, 0, 0.5 * Math.PI);
            cr.arc(x0 + left, top + h - left, left, 0.5 * Math.PI, Math.PI);
            cr.closePath();
            setSource(cr, color, alpha);
            cr.fill();
        };
        segment(0, activeEnd, TRACK_RADIUS, TRACK_INNER_RADIUS, Color.primary, 1);
        segment(inactiveStart, w, TRACK_INNER_RADIUS, TRACK_RADIUS, ...SurfaceEffect[1]);

        // the thumb: a 4 x 52 dp bar
        cr.newPath();
        const tr = THUMB_W / 2;
        cr.arc(c, tr, tr, Math.PI, 2 * Math.PI);
        cr.arc(c, THUMB_H - tr, tr, 0, Math.PI);
        cr.closePath();
        setSource(cr, Color.primary);
        cr.fill();

        // the icon at the end of the inactive segment while it fits there, else at the end of the active one
        const pct = this._value * 100;
        const icon = pct <= 20 ? 'ic_brightness_low' : pct >= 80 ? 'ic_brightness_full' : 'ic_brightness_medium';
        const fitsInactive = TRACK_ICON < w - inactiveStart - 2 * TRACK_ICON_PAD;
        const fitsActive = TRACK_ICON < activeEnd - 2 * TRACK_ICON_PAD;
        if (fitsInactive)
            this._setIconSide(false);
        else if (fitsActive)
            this._setIconSide(true);
        const iy = top + (TRACK_H - TRACK_ICON) / 2;
        if (fitsInactive || this._inactiveIconAlpha > 0) {
            paintIcon(cr, icon, w - TRACK_ICON_PAD - TRACK_ICON, iy, TRACK_ICON, Color.onSurface,
                this._inactiveIconAlpha);
        }
        if (fitsActive && this._activeIconAlpha > 0) {
            paintIcon(cr, icon, activeEnd - TRACK_ICON_PAD - TRACK_ICON, iy, TRACK_ICON, Color.onPrimary,
                this._activeIconAlpha);
        }
    }

    destroy() {
        this._autoSpring?.stop();
        this._iconAnim?.stop();
        super.destroy();
    }
});

// --- footer: settings and power (no foreground-service pill: nothing reports one)

const FooterButton = GObject.registerClass(
class FooterButton extends St.Button {
    constructor(icon, bg, fg, x, name) {
        super({
            x: px(x), y: 0, width: px(FOOTER_BUTTON), height: px(FOOTER_BUTTON),
            reactive: true, can_focus: true, accessible_name: name,
            style: `background-color: ${colorStyle(bg)}; border-radius: ${px(FOOTER_BUTTON / 2)}px;`,
        });
        this.set_child(new VectorIcon(icon, px(FOOTER_ICON), fg));
    }
});

// --- header

const ShadeHeader = GObject.registerClass(
class ShadeHeader extends St.Widget {
    constructor(state) {
        super({width: px(SCREEN_W), height: px(260)});
        this._state = state;

        this._clock = new Clutter.Text({font_name: CLOCK_FONT, color: cogl(Color.white)});
        this._bigClock = new Clutter.Text({font_name: CLOCK_BIG_FONT, color: cogl(Color.white), visible: false});
        this._date = new Clutter.Text({font_name: DATE_FONT, color: cogl(Color.white)});
        this._carrier = new Clutter.Text({font_name: CARRIER_FONT, color: cogl(Color.white), opacity: 0});
        this._icons = new SystemIcons(state, {end: ICONS_QQS[0], centerY: ICONS_QQS[1], height: 116});
        for (const child of [this._icons, this._clock, this._bigClock, this._date, this._carrier])
            this.add_child(child);

        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._syncTime(), this);
        state.connectObject('changed', () => this._syncCarrier(), this);
        this._q = 0;
        this._syncTime();
        this._syncCarrier();
    }

    _syncTime() {
        const now = GLib.DateTime.new_now_local();
        this._clock.text = this._bigClock.text = now.format('%H:%M');
        this._date.text = now.format('%a, %b %-d');
        this._clockInk = inkBox(this._clock);
        this._bigInk = inkBox(this._bigClock);
        this._dateInk = inkBox(this._date);
        this.setProgress(this._q);
    }

    _syncCarrier() {
        const {mobile} = this._state;
        const text = mobile.present ? mobile.operator || '' : 'No SIM';
        if (this._carrier.text !== text) {
            this._carrier.text = text;
            this.setProgress(this._q);
        }
    }

    // q: the QS expansion; MotionLayout's linear interpolation with the keyframes of combined_qs_header_scene.xml
    setProgress(q) {
        this._q = q;
        // the clock grows x2.57 from its ink corner; a crisp small copy at rest
        const [x0, y0] = CLOCK_QQS, [x1, y1] = CLOCK_QS;
        const x = lerp(x0, x1, q), y = lerp(y0, y1, q);
        const scale = lerp(1, CLOCK_SCALE, q);
        this._clock.visible = q <= 0;
        this._bigClock.visible = q > 0;
        if (q <= 0) {
            this._clock.set_position(px(x) - this._clockInk.x, px(y) - this._clockInk.y);
        } else {
            const k = scale / CLOCK_SCALE;
            this._bigClock.set_scale(k, k);
            this._bigClock.set_position(px(x) - this._bigInk.x * k, px(y) - this._bigInk.y * k);
        }

        // date and icons: fade out by 14 %, back in from 58 %, displaced 0.1066 on the way out
        // the spring overshoots q past 1 (and under 0): alpha is kept to 0..1, or opacity 256 throws mid-layout
        const keyed = ([sx, sy], [ex, ey]) => {
            if (q <= HEADER_FADE_OUT) {
                const t = q / HEADER_FADE_OUT;
                return {x: sx, y: sy + t * HEADER_DISPLACEMENT * (ey - sy), alpha: Math.min(1, 1 - t)};
            }
            if (q < HEADER_FADE_IN)
                return {x: ex, y: lerp(sy, ey, q), alpha: 0};
            return {x: ex, y: lerp(sy, ey, q), alpha: Math.min(1, (q - HEADER_FADE_IN) / (1 - HEADER_FADE_IN))};
        };
        const date = keyed(DATE_QQS, DATE_QS);
        this._date.set_position(px(date.x) - this._dateInk.x, px(date.y) - this._dateInk.y);
        this._date.opacity = Math.round(255 * date.alpha);
        const icons = keyed(ICONS_QQS, ICONS_QS);
        this._icons.set_position(px(icons.x - ICONS_QQS[0]), px(icons.y - ICONS_QQS[1]));
        this._icons.opacity = Math.round(255 * icons.alpha);

        // the carrier only fades in, from 58 %
        const ink = inkBox(this._carrier);
        const ctop = lerp(CARRIER_QQS_TOP, CARRIER_QS_TOP, q);
        this._carrier.set_position(px(CARRIER_END) - ink.x - ink.width, px(ctop) - ink.y);
        this._carrier.opacity = Math.round(255 * clamp((q - HEADER_FADE_IN) / (1 - HEADER_FADE_IN)));
    }
});

// --- the panel

export const QSPanel = GObject.registerClass(
class QSPanel extends St.Widget {
    constructor(state, shade) {
        super({width: px(SCREEN_W), height: px(SCREEN_H), clip_to_allocation: false});
        this._state = state;
        this._shade = shade;

        this._header = new ShadeHeader(state);
        this.add_child(this._header);

        this._brightness = new BrightnessSlider(state, shade);
        this.add_child(this._brightness);

        // tiles in reading order; a wide tile that does not fit the row wraps (InfiniteGridLayout.kt)
        this._tiles = [];
        let col = 0, row = 0;
        for (const spec of TILES) {
            const span = spec.wide ? 2 : 1;
            if (col + span > COLUMNS) {
                col = 0;
                row++;
            }
            const tile = new Tile(spec, state, (t, longPress) => shade.onTileActivated?.(t, longPress));
            tile._row = row;
            tile._col = col;
            this._tiles.push(tile);
            this.add_child(tile);
            col += span;
            if (col >= COLUMNS) {
                col = 0;
                row++;
            }
        }

        // the build row: the OS's name where Android shows its build number (tap copies it, as Android does). Not
        // selectable: a Clutter.Text is by default, and a swipe closing the shade from this row painted the selection
        // over it, a pale box.
        const build = GLib.get_os_info('PRETTY_NAME') ?? 'GNOME';
        this._build = new Clutter.Text({font_name: BUILD_FONT, color: cogl(Color.onSurface), text: build,
            reactive: true, selectable: false});
        const copy = new Clutter.ClickGesture();
        copy.connect('recognize', () => {
            St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, build);
        });
        this._build.add_action(copy);
        this.add_child(this._build);

        this._footer = new St.Widget({width: px(SCREEN_W), height: px(FOOTER_BUTTON)});
        const settings = new FooterButton('ic_qs_footer_settings', SurfaceEffect[1], Color.onSurface, SETTINGS_X,
            'Settings');
        settings.connect('clicked', () => {
            shade.close?.();
            openSettings();
        });
        const power = new FooterButton('ic_qs_footer_power', [Color.primary, 1], Color.onPrimary, POWER_X,
            'Power menu');
        power.connect('clicked', () => {
            shade.close?.();
            SystemActions.getDefault().activatePowerOff();
        });
        this._footer.add_child(settings);
        this._footer.add_child(power);
        this.add_child(this._footer);
        addEndMark(this);

        this._f = -1;
        this._q = -1;
        this._squish = -1;
        this.layoutFor({f: 1, q: 0, squish: 1, contentAlpha: 1});
    }

    get brightnessDragging() {
        return this._brightness.dragging;
    }

    // @squish: the tiles' height factor while the shade opens (0.1..1); @contentAlpha: the header's;
    // @mirror: all but the brightness slider fade while it is dragged
    layoutFor({q, squish, contentAlpha, mirror = 0}) {
        const keep = 1 - mirror;
        const shift = (q - 1) * QS_SHIFT;           // everything below the header moves with q
        const content = clamp((q - CONTENT_FADE_FROM) / (1 - CONTENT_FADE_FROM));
        const footer = clamp((q - FOOTER_FADE_FROM) / (1 - FOOTER_FADE_FROM));

        if (q !== this._q)
            this._header.setProgress(q);
        this._header.opacity = Math.round(255 * contentAlpha * keep);

        const tileH = TILE_H * (0.1 + 0.9 * squish);
        for (const tile of this._tiles) {
            const qqs = tile._row < QQS_ROWS;
            // squished rows close up from the top; rows below the QQS follow the QS layout
            const qqsY = GRID_TOP + tile._row * (tileH + ROW_GAP);
            const qsY = GRID_TOP + QS_SHIFT + tile._row * (TILE_H + ROW_GAP);
            const y = qqs ? lerp(qqsY, qsY, q) : qsY + shift;
            tile.set_position(px(GRID_X + tile._col * COL_PITCH), px(y));
            tile.setHeightPx(qqs ? lerp(tileH, TILE_H, q) : TILE_H);
            const alpha = (qqs ? 1 : content) * keep;
            tile.opacity = Math.round(255 * alpha);
            tile.visible = alpha > 0;
        }

        this._brightness.set_position(0, px(SLIDER_CY - THUMB_H / 2 + shift));
        this._brightness.opacity = Math.round(255 * content);
        this._brightness.visible = content > 0;

        const ink = this._buildInk ??= inkBox(this._build);
        this._build.set_position(px(BUILD_INK_X) - ink.x, px(BUILD_INK_TOP + shift) - ink.y);
        this._build.opacity = Math.round(255 * content * keep);
        this._build.visible = content > 0;

        this._footer.set_position(0, px(FOOTER_CY - FOOTER_BUTTON / 2 + shift));
        this._footer.opacity = Math.round(255 * footer * keep);
        this._footer.visible = footer > 0;

        this._q = q;
        this._squish = squish;
    }
});

export {hasIcon};
