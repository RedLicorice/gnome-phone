// The keyguard's bouncers, Android 16's legacy View ones as the reference phone shows them: the PIN pad
// (spec/keyguard-pin.md) and the 3x3 pattern (systemui.md §6.4-6.5, keyguard-extras.md §D.2, §F). Each is a full-screen
// actor in screen px of the 1080x2280 reference that takes the touches on its keys or grid, shows the messages, and
// emits 'submit' with the secret: the PIN's digits, or the pattern's dots 1-9 in the order they were joined (what
// gp-auth checks, as Phosh's pattern lock had it). keyguard.js checks it and calls back wrong(), lockout() or the
// disappear animation.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Graphene from 'gi://Graphene';
import Pango from 'gi://Pango';
import PangoCairo from 'gi://PangoCairo';
import St from 'gi://St';

import {click, DOT_TICK, vibrateRaw} from './haptics.js';
import {Canvas, paintIcon} from './icons.js';
import {Animator, animate, bezier, clamp, FAST_OUT_LINEAR_IN, FAST_OUT_SLOW_IN, lerp, LINEAR,
    LINEAR_OUT_SLOW_IN} from './motion.js';
import {Color, setSource, SurfaceEffect} from './palette.js';
import {AndroidText} from './text.js';
import {px, SCALE, SCREEN_H, SCREEN_W, screenPixels} from './units.js';

const STANDARD = bezier(0.2, 0, 0, 1);
const STANDARD_DECELERATE = bezier(0, 0, 0, 1);
const ACCELERATE_DECELERATE = t => 0.5 - 0.5 * Math.cos(Math.PI * t);

// materialColorPrimaryContainer (tone 30 of the primary's hue), not measured: the pressed key

// --- the messages (keyguard-pin.md §1.1, §4; systemui.md §6.5): primary 18 sp, secondary 14 sp, centred
const MESSAGE_BASELINE = 288;
const SECONDARY_BASELINE = 390;
const SECONDARY_LINE = 56;

export const Strings = {
    pin: {
        prompt: 'Enter PIN',
        wrong: 'Wrong PIN. Try again.',
        tooMany: 'Too many attempts with incorrect PIN',
        restart: 'PIN is required after device restarts',
    },
    pattern: {
        prompt: 'Draw pattern',
        wrong: 'Wrong pattern. Try again.',
        tooMany: 'Too many attempts with incorrect pattern',
        restart: 'Pattern is required after device restarts',
    },
};

export function tryAgainIn(seconds) {
    return seconds === 1 ? 'Try again in 1 second.' : `Try again in ${seconds} seconds.`;
}

// ArgbEvaluator: RGB in linear light, alpha linear
function mixColour([c0, a0], [c1, a1], t) {
    const lin = v => (v / 255) ** 2.2;
    const out = c => Math.round(255 * c ** (1 / 2.2));
    return [c0.map((v, i) => out(lerp(lin(v), lin(c1[i]), t))), lerp(a0, a1, t)];
}

// One line of text centred on @cx with its baseline at @baseline, screen px; text set later
function centredText(parent, sizeSp, wght, cx, baseline) {
    const text = new AndroidText('', sizeSp, wght, {colour: Color.onSurface});
    parent.add_child(text);
    const place = () => {
        text.set_position(px(cx - text.inset - text.textWidth / 2), px(baseline - text.inset - text.baseline));
    };
    return {
        actor: text,
        set: s => {
            // each digit typed sets the prompt again: the same text is not drawn and uploaded again
            if (s === text.shown)
                return;
            text.shown = s;
            text.setText(s);
            text.visible = !!s;
            place();
        },
    };
}

// The primary and the two-line secondary message
class Messages {
    constructor(parent) {
        // A plain actor first, so a message coming or going never changes the parent's first visible child: St
        // restyles the child that gains or loses :first-child, and a restyle redraws every canvas under it
        parent.add_child(new Clutter.Actor());
        this._primary = centredText(parent, 18, 600, 540, MESSAGE_BASELINE);
        this._secondary = [0, 1].map(i => centredText(parent, 14, 600, 540, SECONDARY_BASELINE + i * SECONDARY_LINE));
    }

    set(primary, secondary = '') {
        this._primary.set(primary);
        // two lines at most, broken at a space near the middle when it is long
        let lines = [secondary];
        if (secondary.length > 34) {
            const mid = secondary.lastIndexOf(' ', Math.ceil(secondary.length / 2) + 4);
            if (mid > 0)
                lines = [secondary.slice(0, mid), secondary.slice(mid + 1)];
        }
        this._secondary.forEach((line, i) => line.set(lines[i] ?? ''));
    }

    get actors() {
        return [this._primary.actor, ...this._secondary.map(line => line.actor)];
    }
}

// A touch's slot, the same number for each of its events (GJS's Clutter.EventSequence has no equal()); null for
// the pointer
export function touchSlot(event) {
    return event.get_event_sequence()?.get_slot() ?? null;
}

// Touches and clicks on @actor: onDown(), onUp(inside); a touch that leaves the actor still ends on it
function pressable(actor, onDown, onUp) {
    let down = null;
    actor.reactive = true;
    actor.connect('event', (a, event) => {
        const type = event.type();
        const seq = touchSlot(event);
        if ((type === Clutter.EventType.TOUCH_BEGIN || type === Clutter.EventType.BUTTON_PRESS) && down === null) {
            down = type === Clutter.EventType.TOUCH_BEGIN ? seq : 'pointer';
            onDown();
            return Clutter.EVENT_STOP;
        }
        const mine = type === Clutter.EventType.BUTTON_RELEASE ? down === 'pointer'
            : seq !== null && down !== 'pointer' && down === seq;
        if (!mine)
            return Clutter.EVENT_PROPAGATE;
        if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.BUTTON_RELEASE ||
            type === Clutter.EventType.TOUCH_CANCEL) {
            down = null;
            const [x, y] = event.get_coords();
            const [ok, lx, ly] = actor.transform_stage_point(x, y);
            onUp(type !== Clutter.EventType.TOUCH_CANCEL && ok && lx >= 0 && ly >= 0 &&
                lx <= actor.width && ly <= actor.height);
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_STOP;
    });
}

// =====================================================================================================================
// The PIN pad

const KEY = 252;                                        // 84 dp
const KEY_COLUMNS = [228, 540, 852];
const KEY_ROWS = [852, 1140, 1428, 1716];
const DIGIT_SIZE = 32;                                  // dp, not sp
const ICON = 120;                                       // 40 dp
const DOT_CELL = 90;                                    // 30 dp
const DOT_Y = 574;
const FIELD = [108, 972];
const ROW_SHIFT = [360, 390, 420, 450, 480];            // appear: (120 + 10 i) dp
const APPEAR_MS = 650;

// The keys are squircles, the owner's choice over Android's circles (which square off to h/4 corners pressed):
// superellipses |x/a|^n + |y/b|^n = 1, a little squarer while pressed
const SQUIRCLE_REST = 4;
const SQUIRCLE_PRESSED = 5;
const SQUIRCLE_STEPS = 96;

function squircle(cr, cx, cy, a, b, n) {
    cr.newPath();
    for (let i = 0; i < SQUIRCLE_STEPS; i++) {
        const t = 2 * Math.PI * i / SQUIRCLE_STEPS;
        const c = Math.cos(t), s = Math.sin(t);
        const x = cx + a * Math.sign(c) * Math.abs(c) ** (2 / n);
        const y = cy + b * Math.sign(s) * Math.abs(s) ** (2 / n);
        if (i === 0)
            cr.moveTo(x, y);
        else
            cr.lineTo(x, y);
    }
    cr.closePath();
}

// A key's squircle, drawn once: at rest, or pressed (a little squarer, in the pressed colour). The key animates its
// layers, not their drawings. Every redraw of a canvas costs a fresh texture upload, which Mesa converts pixel by pixel
// on the S10e: the 12 keys redrawn each frame of the appear made a frame take 115 ms.
const KeyShape = GObject.registerClass(
class KeyShape extends Canvas {
    // @effect: [rgb, alpha], read at each drawing (the look changes them in place)
    constructor(effect, squareness) {
        super({width: px(KEY), height: px(KEY)});
        this.set_pivot_point(0.5, 0.5);
        this._effect = effect;
        this._squareness = squareness;
    }

    draw(cr) {
        screenPixels(cr);
        squircle(cr, KEY / 2, KEY / 2, KEY / 2, KEY / 2, this._squareness);
        setSource(cr, ...this._effect);
        cr.fill();
    }
});

// The digit, on a canvas just big enough for it at its widest: it is redrawn while the key is pressed (its colour and
// width follow the press), and a small canvas is a small upload
const DIGIT_FONT = `Google Sans Flex ${DIGIT_SIZE * 3}px @wght=600,wdth=100,ROND=100`;
const DIGIT_WIDEST = 1.35;
let digitBox = null;

// the canvas for a digit, screen px, even so it centres on a whole pixel: the widest digit's logical box, 1.35 wide
function digitCanvasSize() {
    if (!digitBox) {
        const layout = Pango.Layout.new(PangoCairo.FontMap.get_default().create_context());
        layout.set_font_description(Pango.FontDescription.from_string(DIGIT_FONT));
        let w = 0, h = 0;
        for (const digit of '0123456789') {
            layout.set_text(digit, -1);
            const [, logical] = layout.get_pixel_extents();
            w = Math.max(w, logical.width);
            h = Math.max(h, logical.height);
        }
        const even = v => 2 * Math.ceil(v / 2);
        digitBox = [even(w * DIGIT_WIDEST + 12), even(h + 12)];
    }
    return digitBox;
}

const DigitFace = GObject.registerClass(
class DigitFace extends Canvas {
    constructor(digit) {
        const [w, h] = digitCanvasSize();
        super({width: px(w), height: px(h)});
        this.set_position(px((KEY - w) / 2), px((KEY - h) / 2));
        this._w = w;
        this._h = h;
        this._digit = digit;
        this._press = 0;
    }

    set press(v) {
        if (v === this._press)
            return;
        this._press = v;
        this.queue_repaint();
    }

    draw(cr) {
        screenPixels(cr);
        const layout = PangoCairo.create_layout(cr);
        layout.set_font_description(Pango.FontDescription.from_string(DIGIT_FONT));
        layout.set_text(this._digit, -1);
        const [, logical] = layout.get_pixel_extents();
        const [colour] = mixColour([Color.onSurface, 1], [Color.accent1_100, 1], this._press);
        cr.translate(this._w / 2, (this._h - logical.height) / 2);
        cr.scale(lerp(1, DIGIT_WIDEST, this._press), 1);
        cr.moveTo(-logical.width / 2, 0);
        setSource(cr, colour);
        PangoCairo.show_layout(cr, layout);
    }
});

// delete and enter: the icon, which does not change with the press
const IconFace = GObject.registerClass(
class IconFace extends Canvas {
    constructor(icon) {
        super({width: px(ICON), height: px(ICON)});
        this.set_position(px((KEY - ICON) / 2), px((KEY - ICON) / 2));
        this._icon = icon;
    }

    draw(cr) {
        screenPixels(cr);
        paintIcon(cr, this._icon, 0, 0, ICON, Color.onSurface);
    }
});

// A key: the squircle at rest, the pressed one faded in over it, the digit or icon on top
const PinKey = GObject.registerClass(
class PinKey extends St.Widget {
    constructor({digit = null, icon = null, action = false}) {
        super({width: px(KEY), height: px(KEY)});
        this.set_pivot_point(0.5, 0.5);
        this._layers = new St.Widget({width: px(KEY), height: px(KEY)});
        this.add_child(this._layers);
        this._shape = new KeyShape(action ? SurfaceEffect[3] : SurfaceEffect[1], SQUIRCLE_REST);
        this._pressedShape = new KeyShape([Color.primaryContainer, 1], SQUIRCLE_PRESSED);
        this._pressedShape.opacity = 0;
        this._face = icon ? new IconFace(icon) : new DigitFace(digit);
        this._layers.add_child(this._shape);
        this._layers.add_child(this._pressedShape);
        this._layers.add_child(this._face);
        this._press = 0;            // 0 normal .. 1 pressed: the shape, the colours, the digit's width
        this._anim = null;
        this._hidden = false;
    }

    // The appear fades each key: without this, Clutter flattens a fading key's layers into an offscreen buffer of
    // its own every frame (12 of them). Faded layer by layer instead, the digit lets a little of the squircle through
    // while it fades in; nothing anyone can see in 650 ms.
    vfunc_has_overlaps() {
        return false;
    }

    // setProgress: the squircle's height 70 % -> 100 %, a scale of the whole shape as the drawing was
    set appear(s) {
        const y = 0.7 + 0.3 * s;
        this._shape.scale_y = y;
        this._pressedShape.scale_y = y;
    }

    // the enter key while the PIN confirms itself: still there, so nothing around it restyles, but not seen
    get hidden() {
        return this._hidden;
    }

    set hidden(on) {
        this._hidden = on;
        this._layers.opacity = on ? 0 : 255;
    }

    _setPress(v) {
        this._press = v;
        this._pressedShape.opacity = Math.round(255 * v);
        if (this._face instanceof DigitFace)
            this._face.press = v;
    }

    // NumPadAnimator: down runs from its start values in 33 ms LINEAR, up back in 300 ms STANDARD
    animatePress(down) {
        this._anim?.stop();
        const from = down ? 0 : 1, to = down ? 1 : 0;
        this._anim = animate(this, {
            from, to, duration: down ? 33 : 300, interpolator: down ? LINEAR : STANDARD,
            onUpdate: v => this._setPress(v),
            onDone: () => {
                this._anim = null;
            },
        });
    }

    // back at rest at once: a bouncer put away mid-press
    release() {
        this._anim?.stop();
        this._anim = null;
        this._setPress(0);
    }

    destroy() {
        this._anim?.stop();
        super.destroy();
    }
});

// The PIN's shapes (bouncer_shape_1..6, keyguard-pin.md App. A): each pops in at its own shape and relaxes into a
// circle. [base, peak] paths, M and 8 C in the 36-unit viewport around (0, 0); the y offset that centres it
const SHAPES = [
    ['M-8.34 -3.45 C-7.46,-5.58 -5.58,-7.46 -3.45,-8.34 C-1.33,-9.22 1.33,-9.22 3.45,-8.34 C5.58,-7.46 7.46,-5.58 8.34,-3.45 C9.22,-1.33 9.22,1.33 8.34,3.45 C7.46,5.58 5.58,7.46 3.45,8.34 C1.33,9.22 -1.33,9.22 -3.45,8.34 C-5.58,7.46 -7.46,5.58 -8.34,3.45 C-9.22,1.33 -9.22,-1.33 -8.34,-3.45',
        'M-13.65 -3.95 C-16.31,-10.09 -10.09,-16.31 -3.95,-13.65 C-0.76,-12.26 0.77,-12.23 3.95,-13.65 C9.91,-16.31 16.31,-10.09 13.65,-3.95 C12.26,-0.76 12.26,0.76 13.65,3.95 C16.31,10.09 9.91,16.31 3.95,13.65 C0.76,12.22 -0.76,12.26 -3.95,13.65 C-10.09,16.31 -16.31,10.09 -13.65,3.95 C-12.26,0.75 -12.25,-0.73 -13.65,-3.95', 18],
    ['M9 0 C9,-2.3 7.99,-4.74 6.36,-6.36 C4.74,-7.99 2.3,-9 0,-9 C-2.3,-9 -4.74,-7.99 -6.36,-6.36 C-7.99,-4.74 -9,-2.3 -9,0 C-9,2.3 -7.99,4.74 -6.36,6.36 C-4.74,7.99 -2.3,9 0,9 C2.3,9 4.74,7.99 6.36,6.36 C7.99,4.74 9,2.3 9,0',
        'M15.06 7.12 C15.06,7.12 5.57,-11.84 5.57,-11.84 C4.43,-14.13 2.23,-15.28 0.02,-15.28 C-2.18,-15.28 -4.39,-14.13 -5.53,-11.84 C-5.53,-11.84 -15.01,7.12 -15.01,7.12 C-17.62,12.34 -12.27,18.03 -6.91,15.56 C-3.18,13.84 -1.9,13.04 0.02,13.04 C1.94,13.04 4.59,14.43 6.96,15.56 C12.14,18.03 17.67,12.34 15.06,7.12', 18],
    ['M6.35 6.37 C2.84,9.88 -2.85,9.88 -6.36,6.37 C-6.36,6.37 -6.37,6.37 -6.37,6.37 C-9.88,2.85 -9.88,-2.84 -6.37,-6.35 C-2.85,-9.86 2.84,-9.86 6.35,-6.35 C6.35,-6.35 6.35,-6.34 6.35,-6.34 C9.86,-2.83 9.86,2.86 6.35,6.37',
        'M11.96 11.96 C6.61,17.31 -2.06,17.31 -7.41,11.96 C-7.41,11.96 -11.96,7.42 -11.96,7.42 C-17.31,2.06 -17.31,-6.61 -11.96,-11.96 C-6.61,-17.31 2.06,-17.31 7.42,-11.96 C7.42,-11.96 11.96,-7.41 11.96,-7.41 C17.31,-2.06 17.31,6.61 11.96,11.96', 18],
    ['M8.98 -0.01 C9,-1.16 8.76,-2.33 8.32,-3.4 C7.86,-4.49 7.18,-5.52 6.35,-6.36 C5.55,-7.18 4.58,-7.84 3.53,-8.27 C2.42,-8.74 1.2,-9 0,-9 C-1.19,-9 -2.4,-8.74 -3.5,-8.29 C-4.56,-7.85 -5.54,-7.18 -6.35,-6.36 C-7.16,-5.56 -7.83,-4.58 -8.27,-3.52 C-8.73,-2.42 -8.96,-1.2 -8.98,-0.01 C-9,1.09 -8.8,2.21 -8.39,3.22 C-7.91,4.37 -7.22,5.45 -6.35,6.35 C-5.56,7.17 -4.56,7.83 -3.51,8.27 C-2.41,8.73 -1.19,8.97 0,8.98 C1.1,9 2.24,8.8 3.26,8.37 C4.39,7.9 5.45,7.2 6.35,6.35 C7.13,5.61 7.77,4.69 8.18,3.7 C8.67,2.53 8.96,1.26 8.98,-0.01',
        'M15.94 0 C15.94,-2.68 12.74,-2.45 11.62,-4.58 C10.5,-6.72 13.17,-9.54 11.27,-11.27 C9.37,-13 6.84,-10.78 4.74,-11.54 C2.63,-12.3 2.77,-15.94 0,-15.94 C-2.77,-15.94 -2.85,-12.34 -4.73,-11.52 C-6.62,-10.71 -9.31,-13.21 -11.27,-11.27 C-13.23,-9.33 -10.95,-7.11 -11.55,-4.93 C-12.15,-2.77 -15.94,-2.78 -15.94,0 C-15.94,2.78 -12.4,2.92 -11.62,4.69 C-10.85,6.47 -13.32,9.29 -11.27,11.27 C-9.22,13.25 -6.57,10.83 -4.82,11.64 C-3.07,12.46 -2.63,15.94 0,15.94 C2.63,15.94 2.64,12.52 4.72,11.67 C6.8,10.82 9.66,13.2 11.27,11.27 C12.88,9.34 10.62,6.93 11.65,4.63 C12.68,2.34 15.94,2.68 15.94,0', 18],
    ['M-2.78 -9.43 C-1.02,-10 1.02,-10 2.78,-9.43 C4.54,-8.85 6.19,-7.65 7.28,-6.15 C8.37,-4.65 9,-2.71 9,-0.86 C9,1 8.37,2.93 7.28,4.43 C6.19,5.94 4.55,7.13 2.78,7.71 C1.02,8.28 -1.02,8.28 -2.78,7.71 C-4.54,7.13 -6.19,5.94 -7.28,4.43 C-8.37,2.93 -9,1 -9,-0.86 C-9,-2.71 -8.37,-4.65 -7.28,-6.15 C-6.19,-7.65 -4.55,-8.85 -2.78,-9.43',
        'M-3.08 -15.05 C-1.24,-16.37 1.24,-16.37 3.08,-15.05 C3.08,-15.05 13.71,-7.34 13.71,-7.34 C15.57,-5.99 16.34,-3.59 15.63,-1.4 C15.63,-1.4 11.59,11.01 11.59,11.01 C10.88,13.18 8.86,14.65 6.59,14.66 C6.59,14.66 -6.59,14.66 -6.59,14.66 C-8.86,14.65 -10.88,13.18 -11.59,11.01 C-11.59,11.01 -15.63,-1.4 -15.63,-1.4 C-16.34,-3.59 -15.57,-5.99 -13.71,-7.34 C-13.71,-7.34 -3.08,-15.05 -3.08,-15.05', 18.859],
    ['M8.33 -4.31 C7.45,-6.43 5.58,-8.31 3.45,-9.19 C1.33,-10.07 -1.33,-10.07 -3.45,-9.19 C-5.58,-8.31 -7.45,-6.43 -8.33,-4.31 C-9.21,-2.18 -9.21,0.47 -8.33,2.59 C-7.45,4.72 -5.58,6.59 -3.45,7.47 C-1.33,8.35 1.33,8.35 3.45,7.47 C5.58,6.59 7.45,4.72 8.33,2.59 C9.21,0.47 9.21,-2.18 8.33,-4.31',
        'M14.18 -5.39 C14.18,-5.39 4.53,-15.04 4.53,-15.04 C2.03,-17.55 -2.03,-17.55 -4.53,-15.04 C-4.53,-15.04 -14.18,-5.39 -14.18,-5.39 C-16.69,-2.89 -16.69,1.17 -14.18,3.67 C-14.18,3.67 -4.53,13.32 -4.53,13.32 C-2.03,15.83 2.03,15.83 4.53,13.32 C4.53,13.32 14.18,3.67 14.18,3.67 C16.69,1.17 16.69,-2.89 14.18,-5.39', 18.859],
].map(([base, peak, oy]) => {
    const nums = d => d.match(/-?[0-9]*\.?[0-9]+/g).map(Number);
    return {base: nums(base), peak: nums(peak), oy};
});

const SHAPE_GROUP_SCALE = bezier(0.001, 0, 0.5, 1);
const SHAPE_TO_PEAK = bezier(0.167, 0.167, 0.833, 0.833);
const SHAPE_TO_BASE = bezier(0.7, 0, 0.6, 1);

// One entered digit, in a 30 dp cell
const PinDot = GObject.registerClass(
class PinDot extends Canvas {
    constructor(shape) {
        super({width: px(DOT_CELL), height: px(DOT_CELL)});
        this.set_pivot_point(0.5, 0.5);
        this._shape = shape;
        this._t = 0;
        this._anim = null;
    }

    // the pop-in, once on the stage: a timeline whose actor is not on one does not start
    pop() {
        this._anim = animate(this, {
            from: 0, to: 350, duration: 350, interpolator: LINEAR,
            onUpdate: v => {
                this._t = v;
                this.queue_repaint();
            },
            onDone: () => {
                this._anim = null;
            },
        });
    }

    draw(cr) {
        screenPixels(cr);
        const t = this._t;
        // the group scale over 0-67 ms; the path base, base -> peak over 33-67, peak -> base over 67-350
        const scale = SHAPE_GROUP_SCALE(clamp(t / 67));
        let path;
        if (t < 67)
            path = this._shape.base.map((v, i) => lerp(v, this._shape.peak[i], SHAPE_TO_PEAK(clamp((t - 33) / 34))));
        else
            path = this._shape.peak.map((v, i) => lerp(v, this._shape.base[i], SHAPE_TO_BASE(clamp((t - 67) / 283))));
        if (scale <= 0)
            return;
        // the 36-unit drawable fit into the 90 px cell, its centre at (18, 18)
        cr.translate(DOT_CELL / 2, DOT_CELL / 2);
        cr.scale(DOT_CELL / 36 * scale, DOT_CELL / 36 * scale);
        cr.translate(0, this._shape.oy - 18);
        cr.moveTo(path[0], path[1]);
        for (let i = 2; i + 5 < path.length; i += 6)
            cr.curveTo(path[i], path[i + 1], path[i + 2], path[i + 3], path[i + 4], path[i + 5]);
        cr.closePath();
        setSource(cr, Color.onSurface);
        cr.fill();
    }

    destroy() {
        this._anim?.stop();
        super.destroy();
    }
});

export const PinView = GObject.registerClass({
    Signals: {'submit': {param_types: [GObject.TYPE_STRING]}},
}, class PinView extends St.Widget {
    constructor() {
        super({width: px(SCREEN_W), height: px(SCREEN_H)});
        this.kind = 'pin';
        this._digits = [];
        this._dots = [];
        this._enabled = true;
        this._shapes = SHAPES.map(s => [Math.random(), s]).sort((a, b) => a[0] - b[0]).map(([, s]) => s);

        this._messages = new Messages(this);
        // the entry field, clipped to its width
        this._field = new St.Widget({x: px(FIELD[0]), y: px(DOT_Y - DOT_CELL / 2), width: px(FIELD[1] - FIELD[0]),
            height: px(DOT_CELL), clip_to_allocation: true});
        this.add_child(this._field);

        this._keys = [];
        this._rows = [[this._field], [], [], [], []];
        const layout = [['1', '2', '3'], ['4', '5', '6'], ['7', '8', '9'], ['delete', '0', 'enter']];
        layout.forEach((row, r) => row.forEach((name, c) => {
            const key = name === 'delete' ? new PinKey({icon: 'keyguard_pin_bouncer_delete_outline', action: true})
                : name === 'enter' ? new PinKey({icon: 'keyguard_pin_bouncer_confirm', action: true})
                    : new PinKey({digit: name});
            key.set_position(px(KEY_COLUMNS[c] - KEY / 2), px(KEY_ROWS[r] - KEY / 2));
            key.name = name;
            this.add_child(key);
            this._rows[r + 1].push(key);
            this._keys.push(key);
            this._wireKey(key);
        }));
        // the shake's order: 1-9, delete, 0, enter
        this._shakeOrder = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'delete', '0', 'enter']
            .map(n => this._keys.find(k => k.name === n));
    }

    _wireKey(key) {
        let longPress = 0;
        pressable(key, () => {
            if (key.hidden)
                return;
            // NumPadKey / NumPadButton: doHapticKeyClick (VIRTUAL_KEY) as the finger lands, the digits', delete's and
            // enter's alike
            click();
            key.animatePress(true);
            if (key.name === 'delete') {
                longPress = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                    longPress = 0;
                    this.reset(true);
                    click();            // the delete key's long click, as KeyguardPinBasedInputView's
                    return GLib.SOURCE_REMOVE;
                });
            }
        }, inside => {
            if (key.hidden)
                return;
            key.animatePress(false);
            if (longPress) {
                GLib.source_remove(longPress);
                longPress = 0;
            } else if (key.name === 'delete') {
                return;                 // the long press cleared it
            }
            if (inside)
                this._click(key.name);
        });
    }

    _click(name) {
        if (name === 'delete') {
            this._delete();
            this.emit('submit', null);  // input: the message goes back to the prompt
            return;
        }
        if (!this._enabled)
            return;
        if (name === 'enter') {
            this._confirm(false);
            return;
        }
        this._append(name);
    }

    // the PIN goes to keyguard.js: from enter, or by itself once it is as long as the PIN (@auto)
    _confirm(auto) {
        this._enabled = false;
        const pin = this._digits.join('');
        this._digits = [];
        this.autoConfirmed = auto;
        this.emit('submit', pin);
    }

    // Android 14's auto-confirm (LockSettings' PIN length, KeyguardPinViewController): with the PIN's length known and
    // 6 or more, the PIN goes as its last digit is typed, and the enter key is not shown. 0 turns it off
    set autoConfirm(length) {
        this._autoLength = length;
        this._keys.find(k => k.name === 'enter').hidden = length > 0;
    }

    // --- the dots

    _dotX(k, n) {
        // centred; ten and more sit flush with the field's end, the oldest cut off on the left
        return n >= 10 ? FIELD[1] - DOT_CELL * (n - k) : 540 - DOT_CELL / 2 * n + DOT_CELL * k;
    }

    _layoutDots(animated) {
        const n = this._dots.length;
        this._dots.forEach((dot, k) => {
            const x = px(this._dotX(k, n) - FIELD[0]);
            dot.slide?.stop();
            if (!animated || dot.fresh) {
                dot.fresh = false;
                dot.x = x;
                return;
            }
            const from = dot.x;
            dot.slide = animate(dot, {from, to: x, duration: 160, interpolator: LINEAR_OUT_SLOW_IN,
                onUpdate: v => {
                    dot.x = v;
                }});
        });
    }

    _append(digit) {
        this._digits.push(digit);
        const dot = new PinDot(this._shapes[this._dots.length % this._shapes.length]);
        dot.fresh = true;
        this._dots.push(dot);
        this._field.add_child(dot);
        dot.pop();
        this._layoutDots(true);
        this.emit('submit', null);
        if (this._autoLength && this._digits.length === this._autoLength)
            this._confirm(true);
    }

    _delete() {
        this._digits.pop();
        const dot = this._dots.pop();
        if (!dot)
            return;
        // a delete running ends at once
        this._deleting?.();
        let done = false;
        const finish = () => {
            if (done)
                return;
            done = true;
            this._deleting = null;
            anim?.stop();
            dot.slide?.stop();
            dot.destroy();
            this._layoutDots(true);
        };
        this._deleting = finish;
        const anim = animate(dot, {from: 1, to: 0, duration: 160, interpolator: ACCELERATE_DECELERATE,
            onUpdate: v => dot.set_scale(v, v), onDone: finish});
    }

    // everything entered goes, right to left with a stagger
    reset(animated) {
        this._digits = [];
        this._resetTimers?.forEach(id => GLib.source_remove(id));
        this._resetTimers = [];
        const n = this._dots.length;
        if (!animated || n === 0) {
            this._dots.forEach(d => {
                d.slide?.stop();
                d.destroy();
            });
            this._dots = [];
            return;
        }
        const base = Math.min(Math.floor(200 / n), 40);
        for (let i = 0; i < n; i++) {
            const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.floor(base * i), () => {
                this._resetTimers = this._resetTimers.filter(t => t !== id);
                this._delete();
                return GLib.SOURCE_REMOVE;
            });
            this._resetTimers.push(id);
        }
    }

    // --- what keyguard.js tells it

    setMessage(primary, secondary) {
        this._messages.set(primary, secondary);
    }

    // input back on after a check
    setEnabled(on) {
        this._enabled = on;
    }

    // a wrong PIN, too short or checked: the dots clear, the keys shake
    wrong() {
        this.reset(true);
        this._shakeOrder.forEach((key, i) => {
            key.shake?.stop();
            key.shake = animate(key, {from: 0, to: 717, duration: 717 + 33 * i, interpolator: LINEAR,
                onUpdate: (v, f) => {
                    const t = f * (717 + 33 * i) - 33 * i;
                    const s = t <= 0 ? 1 : t < 50 ? lerp(1, 0.8, STANDARD(t / 50))
                        : lerp(0.8, 1, STANDARD(clamp((t - 50) / 617)));
                    key.set_scale(s, s);
                }});
        });
    }

    // KeyguardPINView.startAppearAnimation: rows fade and rise over 650 ms
    appear() {
        this._appearAnim?.stop();
        const apply = t => {
            const p = ACCELERATE_DECELERATE(t);
            const sp = STANDARD_DECELERATE(p);
            this._rows.forEach((row, i) => {
                const s = LINEAR_OUT_SLOW_IN(clamp((p - 0.075 * i) / 0.55));
                for (const actor of row) {
                    actor.opacity = Math.round(255 * s);
                    actor.translation_y = px(ROW_SHIFT[i] * (1 - sp));
                    if (actor instanceof PinKey)
                        actor.appear = s;
                }
            });
        };
        apply(0);
        this._appearAnim = animate(this, {from: 0, to: 1, duration: APPEAR_MS, interpolator: LINEAR,
            onUpdate: v => apply(v), onDone: () => {
                this._appearAnim = null;
            }});
    }

    // on the right PIN: the whole view fades and rises 50 dp in 200 ms
    disappear(onDone) {
        this._disappearAnim = animate(this, {from: 0, to: 1, duration: 200, interpolator: FAST_OUT_LINEAR_IN,
            onUpdate: v => {
                this.opacity = Math.round(255 * (1 - v));
                this.translation_y = px(-150 * v);
            }, onDone: () => {
                this._disappearAnim = null;
                onDone?.();
            }});
    }

    // put away until the next time: the keyguard keeps one bouncer, drawn while the screen goes off, rather than
    // building and drawing one under the finger of every swipe up
    park() {
        this._appearAnim?.stop();
        this._appearAnim = null;
        this._disappearAnim?.stop();
        this._disappearAnim = null;
        this.reset(false);
        for (const key of this._keys) {
            key.shake?.stop();
            key.shake = null;
            key.set_scale(1, 1);
            key.release();
        }
        this._rows.forEach(row => row.forEach(actor => {
            actor.opacity = 255;
            actor.translation_y = 0;
            if (actor instanceof PinKey)
                actor.appear = 1;
        }));
        this.opacity = 255;
        this.translation_y = 0;
        this._enabled = true;
    }

    // a touch on the keys or the field is the bouncer's own; elsewhere it may be a swipe
    ownsPoint(x, y) {
        return y >= px(DOT_Y - DOT_CELL / 2) && y <= px(KEY_ROWS[3] + KEY / 2) &&
            x >= px(KEY_COLUMNS[0] - KEY / 2) && x <= px(KEY_COLUMNS[2] + KEY / 2);
    }

    destroy() {
        this._appearAnim?.stop();
        this._disappearAnim?.stop();
        this._resetTimers?.forEach(id => GLib.source_remove(id));
        this._resetTimers = [];
        super.destroy();
    }
});

// =====================================================================================================================
// The pattern

const CELL_X = [192, 540, 888];
const CELL_Y = [972, 1320, 1668];
const PITCH = 348;
const DOT = 48;                     // 16 dp
const DOT_ACTIVE = 72;              // 24 dp
const LINE = 96;                    // 32 dp
const HIT_RADIUS = PITCH * 0.6 / 2;
const MIN_PATTERN = 4;              // dots: fewer is not an attempt
const CLEAR_MS = 2000;
const SEGMENT_FADE_DELAY = 150;
const SEGMENT_FADE_MS = 500;

// A bar with round ends from (x0, y0) to (x1, y1), screen px; an St.Widget turned on its pivot, nothing to draw
function placeBar(bar, x0, y0, x1, y1) {
    const length = Math.hypot(x1 - x0, y1 - y0);
    bar.set_size(px(length + LINE), px(LINE));
    bar.set_pivot_point(LINE / 2 / (length + LINE), 0.5);
    bar.set_position(px(x0 - LINE / 2), px(y0 - LINE / 2));
    bar.rotation_angle_z = Math.atan2(y1 - y0, x1 - x0) * 180 / Math.PI;
}

function barStyle(colour, alpha) {
    return `background-color: rgba(${colour.join(', ')}, ${alpha}); border-radius: ${px(LINE / 2)}px;`;
}

export const PatternView = GObject.registerClass({
    Signals: {'submit': {param_types: [GObject.TYPE_STRING]}},
}, class PatternView extends St.Widget {
    constructor() {
        super({width: px(SCREEN_W), height: px(SCREEN_H)});
        this.kind = 'pattern';
        this._messages = new Messages(this);
        this._enabled = true;
        this._wrong = false;
        this._pattern = [];         // cell indices 0-8
        this._segments = [];        // {bar, done (ms) or null}
        this._clearId = 0;

        // the grid, so it can rise as one
        this._grid = new St.Widget({width: px(SCREEN_W), height: px(SCREEN_H)});
        this.add_child(this._grid);
        this._lines = new St.Widget({width: px(SCREEN_W), height: px(SCREEN_H)});
        this._grid.add_child(this._lines);
        this._cells = [];
        for (let i = 0; i < 9; i++) {
            const cell = new St.Widget({width: px(DOT_ACTIVE), height: px(DOT_ACTIVE),
                pivot_point: new Graphene.Point({x: 0.5, y: 0.5})});
            const dot = new St.Widget({width: px(DOT_ACTIVE), height: px(DOT_ACTIVE),
                pivot_point: new Graphene.Point({x: 0.5, y: 0.5})});
            cell.add_child(dot);
            cell.set_position(px(CELL_X[i % 3] - DOT_ACTIVE / 2), px(CELL_Y[Math.floor(i / 3)] - DOT_ACTIVE / 2));
            this._grid.add_child(cell);
            this._cells.push({cell, dot, size: DOT, colour: 0, anim: null});
            this._styleDot(i);
        }
        this._current = new St.Widget({visible: false});
        this._lines.add_child(this._current);

        this.reactive = true;
        this._down = null;
        this.connect('event', (a, event) => this._onEvent(event));
        this._frames = new Animator(this, () => this._fadeSegments());
    }

    // --- the dots

    _styleDot(i) {
        const c = this._cells[i];
        const [colour, alpha] = this._wrong && this._pattern.includes(i) ? [[0, 0, 0], 1]
            : mixColour([Color.onSurface, 1], [Color.onPrimary, 1], c.colour);
        c.dot.style = `background-color: rgba(${colour.join(', ')}, ${alpha}); border-radius: ${px(DOT_ACTIVE / 2)}px;`;
        const s = c.size / DOT_ACTIVE;
        c.dot.set_scale(s, s);
    }

    // a dot grows (96 ms LOSI) and takes the active colour (50 ms FOSI); it shrinks back (192 ms FOSI, colour 50 ms
    // LOSI) when the next is joined
    _animateDot(i, active) {
        const c = this._cells[i];
        c.anim?.stop();
        const from = c.size, to = active ? DOT_ACTIVE : DOT, fromColour = c.colour, toColour = active ? 1 : 0;
        const ms = active ? 96 : 192;
        c.anim = animate(c.dot, {from: 0, to: ms, duration: ms, interpolator: LINEAR,
            onUpdate: elapsed => {
                c.size = lerp(from, to, (active ? LINEAR_OUT_SLOW_IN : FAST_OUT_SLOW_IN)(clamp(elapsed / ms)));
                c.colour = lerp(fromColour, toColour,
                    (active ? FAST_OUT_SLOW_IN : LINEAR_OUT_SLOW_IN)(clamp(elapsed / 50)));
                this._styleDot(i);
            },
            onDone: () => {
                c.anim = null;
            }});
    }

    // --- touches

    _onEvent(event) {
        const type = event.type();
        const seq = touchSlot(event);
        const begin = type === Clutter.EventType.TOUCH_BEGIN || type === Clutter.EventType.BUTTON_PRESS;
        const isTouch = type >= Clutter.EventType.TOUCH_BEGIN && type <= Clutter.EventType.TOUCH_CANCEL;
        if (begin) {
            if (this._down !== null || !this._enabled)
                return Clutter.EVENT_PROPAGATE;
            const [x, y] = this._local(event);
            if (!this.ownsPoint(px(x), px(y)))
                return Clutter.EVENT_PROPAGATE;
            this._down = isTouch ? seq : 'pointer';
            this._start();
            this._move(x, y);
            return Clutter.EVENT_STOP;
        }
        const mine = this._down !== null && (isTouch ? this._down !== 'pointer' && this._down === seq
            : this._down === 'pointer' && (type === Clutter.EventType.MOTION || type === Clutter.EventType.BUTTON_RELEASE));
        if (!mine)
            return Clutter.EVENT_PROPAGATE;
        const [x, y] = this._local(event);
        if (type === Clutter.EventType.TOUCH_UPDATE || type === Clutter.EventType.MOTION) {
            this._move(x, y);
        } else {
            this._down = null;
            this._finish(type !== Clutter.EventType.TOUCH_CANCEL);
        }
        return Clutter.EVENT_STOP;
    }

    // screen px of the reference
    _local(event) {
        const [sx, sy] = event.get_coords();
        const [, x, y] = this.transform_stage_point(sx, sy);
        return [x * SCALE, y * SCALE];
    }

    _start() {
        this._clear();
        this._wrong = false;
        this.emit('submit', null);
    }

    _hit(x, y) {
        for (let i = 0; i < 9; i++) {
            if (!this._pattern.includes(i) &&
                Math.hypot(x - CELL_X[i % 3], y - CELL_Y[Math.floor(i / 3)]) <= HIT_RADIUS)
                return i;
        }
        return -1;
    }

    _centre(i) {
        return [CELL_X[i % 3], CELL_Y[Math.floor(i / 3)]];
    }

    _join(i) {
        const last = this._pattern.at(-1);
        // a dot passed over on the way is joined too
        if (last !== undefined) {
            const [r0, c0, r1, c1] = [Math.floor(last / 3), last % 3, Math.floor(i / 3), i % 3];
            if (Math.abs(r1 - r0) !== 1 && Math.abs(c1 - c0) !== 1 &&
                (r1 - r0) % 2 === 0 && (c1 - c0) % 2 === 0) {
                const mid = (r0 + r1) / 2 * 3 + (c0 + c1) / 2;
                if (!this._pattern.includes(mid))
                    this._join(mid);
            }
        }
        const prev = this._pattern.at(-1);
        this._pattern.push(i);
        this._animateDot(i, true);
        if (prev !== undefined) {
            this._animateDot(prev, false);
            const bar = new St.Widget({style: barStyle(Color.primary, 1)});
            placeBar(bar, ...this._centre(prev), ...this._centre(i));
            this._lines.insert_child_below(bar, this._current);
            this._segments.push({bar, done: GLib.get_monotonic_time() / 1000});
        }
    }

    _move(x, y) {
        const i = this._hit(x, y);
        // LockPatternView.detectAndAddHit: VIRTUAL_KEY once per hit, a dot passed over on the way included; twice the
        // click's strength (haptics.js)
        if (i >= 0) {
            this._join(i);
            vibrateRaw(DOT_TICK);
        }
        // the line from the last dot to the finger, fading in as it leaves the dot
        const last = this._pattern.at(-1);
        if (last === undefined) {
            this._current.hide();
            return;
        }
        const [cx, cy] = this._centre(last);
        const alpha = clamp((Math.hypot(x - cx, y - cy) / PITCH - 0.3) * 4);
        this._current.style = barStyle(Color.primary, alpha);
        placeBar(this._current, cx, cy, x, y);
        this._current.show();
    }

    // the joined segments fade 150 ms after they were drawn, over 500 ms, while the finger is down
    _fadeSegments() {
        if (this._down === null || this._wrong)
            return true;
        const now = GLib.get_monotonic_time() / 1000;
        for (const s of this._segments) {
            const a = 1 - clamp((now - s.done - SEGMENT_FADE_DELAY) / SEGMENT_FADE_MS);
            if (a !== s.alpha) {
                s.alpha = a;
                s.bar.style = barStyle(Color.primary, a);
            }
        }
        return true;
    }

    _finish(lifted) {
        this._current.hide();
        if (!lifted || this._pattern.length === 0) {
            this._clear();
            return;
        }
        const pattern = this._pattern.map(i => i + 1).join('');
        if (this._pattern.length < MIN_PATTERN) {
            this.wrong();
            return;
        }
        this._enabled = false;
        this.emit('submit', pattern);
    }

    _clear() {
        if (this._clearId)
            GLib.source_remove(this._clearId);
        this._clearId = 0;
        for (const s of this._segments)
            s.bar.destroy();
        this._segments = [];
        const joined = this._pattern;
        this._pattern = [];
        this._wrong = false;
        for (const i of joined) {
            this._cells[i].anim?.stop();
            this._cells[i].size = DOT;
            this._cells[i].colour = 0;
            this._styleDot(i);
        }
    }

    // --- what keyguard.js tells it

    setMessage(primary, secondary) {
        this._messages.set(primary, secondary);
    }

    setEnabled(on) {
        this._enabled = on;
        if (!on)
            this._clear();
    }

    reset() {
        this._clear();
    }

    // DisplayMode.Wrong: with the revamped bouncer its colour is 0, the path and the dots go opaque black; cleared
    // after 2 s
    wrong() {
        this._wrong = true;
        for (const s of this._segments)
            s.bar.style = barStyle([0, 0, 0], 1);
        for (const i of this._pattern)
            this._styleDot(i);
        if (this._clearId)
            GLib.source_remove(this._clearId);
        this._clearId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CLEAR_MS, () => {
            this._clearId = 0;
            this._clear();
            return GLib.SOURCE_REMOVE;
        });
    }

    // KeyguardPatternView.startAppearAnimation: the view rises 48 dp in 500 ms LOSI; each dot grows and rises in
    // 220 ms LOSI, staggered diagonally
    appear() {
        this._appearAnim?.stop();
        const delay = (r, c) => (r * 40 + c * (r ** 0.4 + 0.4) * 20) * 2;
        const apply = ms => {
            this._grid.translation_y = px(144 * (1 - LINEAR_OUT_SLOW_IN(clamp(ms / 500))));
            this._cells.forEach(({cell}, i) => {
                const t = LINEAR_OUT_SLOW_IN(clamp((ms - delay(Math.floor(i / 3), i % 3)) / 220));
                cell.set_scale(t, t);
                cell.translation_y = px(144 * (1 - t));
            });
        };
        apply(0);
        this._appearAnim = animate(this, {from: 0, to: 600, duration: 600, interpolator: LINEAR,
            onUpdate: v => apply(v), onDone: () => {
                this._appearAnim = null;
            }});
    }

    // KeyguardPatternView.startDisappearAnimation: the view rises 38.4 dp in 300 ms; the dots fade in 125 ms as they
    // rise, the top row furthest; done when the last dot is (≈217 ms)
    disappear(onDone) {
        const delay = (r, c) => (r * 60 + c * (r ** 0.4 + 0.4) * 10) * 0.6;
        let finished = false;
        this._disappearAnim = animate(this, {from: 0, to: 300, duration: 300, interpolator: LINEAR,
            onUpdate: ms => {
                this._grid.translation_y = px(-115.2 * FAST_OUT_LINEAR_IN(ms / 300));
                this._lines.opacity = Math.round(255 * (1 - FAST_OUT_LINEAR_IN(clamp(ms / 125))));
                this._cells.forEach(({cell}, i) => {
                    const r = Math.floor(i / 3);
                    const t = FAST_OUT_LINEAR_IN(clamp((ms - delay(r, i % 3)) / 125));
                    cell.opacity = Math.round(255 * (1 - t));
                    cell.translation_y = px(-((3 - r) ** 2 / 3) * 115.2 * t);
                });
                if (!finished && ms >= delay(2, 2) + 125) {
                    finished = true;
                    onDone?.();
                }
            },
            onDone: () => {
                this._disappearAnim = null;
            }});
    }

    // put away until the next time (see PinView.park)
    park() {
        this._appearAnim?.stop();
        this._appearAnim = null;
        this._disappearAnim?.stop();
        this._disappearAnim = null;
        this._down = null;
        this._clear();
        this._grid.translation_y = 0;
        this._lines.opacity = 255;
        this._cells.forEach(({cell}) => {
            cell.opacity = 255;
            cell.translation_y = 0;
            cell.set_scale(1, 1);
        });
        this.opacity = 255;
        this.translation_y = 0;
        this._enabled = true;
    }

    // the pattern has no length to confirm by
    set autoConfirm(_length) {
    }

    ownsPoint(x, y) {
        const half = PITCH * 1.5;
        return x >= px(540 - half) && x <= px(540 + half) && y >= px(1320 - half) && y <= px(1320 + half);
    }

    destroy() {
        this._appearAnim?.stop();
        this._disappearAnim?.stop();
        this._frames.stop();
        if (this._clearId)
            GLib.source_remove(this._clearId);
        this._clearId = 0;
        super.destroy();
    }
});
