// The lock screen's fingerprint hint: the sensor's glyph in the power key and one line of words, on the right edge at
// the key's height (finger.js holds the states and the words; this file draws them and listens to fprintd).
//
// fprintd is what makes "lettura in corso" and "non riconosciuto" appear while a run is still going: its device
// object's VerifyStatus signals say what the sensor is doing, the run's exit status says only how it ended.
//
// The same edge carries the way to the secret while a window covers the lock screen (PinButton): the hint says why the
// sensor is not the way in, the button is the way in.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {fingerHint, secretWord, VERIFY_STATES} from './finger.js';
import {AndroidText} from './text.js';
import {Color, rgba} from './palette.js';
import {px, SCREEN_H, SCREEN_W} from './units.js';

// The hint's place: the right edge at the screen's middle, where the power key and its sensor are, clear of the lock
// icon far below and of the bouncer. The 1080x2280 reference's pixels, as everything on this screen.
const HINT = {right: 48, centreY: Math.round(SCREEN_H / 2), glyph: 54, gap: 12, size: 14, wght: 500};

// The button's place: the same edge, under the hint, its word a tint of the theme's surface on the wall, sized to the
// word with the padding of a touch target around it (48 dp wide, 32 dp high).
const BUTTON = {right: 48, top: Math.round(SCREEN_H / 2) + 132, height: 96, pad: 40, inkTop: 28, size: 15,
    wght: 600, radius: 48, tint: 0.32};

export const FingerHint = GObject.registerClass(
class FingerHint extends St.Widget {
    constructor() {
        super({width: px(SCREEN_W), height: px(SCREEN_H), reactive: false, visible: false});
        this._glyph = new St.Icon({icon_name: 'auth-fingerprint-symbolic', icon_size: px(HINT.glyph)});
        this.add_child(this._glyph);
        this._label = new AndroidText('', HINT.size, HINT.wght, {colour: Color.onWall, opsz: 16});
        this.add_child(this._label);
        this.restyle();
    }

    // the look is live: the glyph takes the accent from the theme, the line keeps Color.onWall, which palette.js
    // mutates in place (a restyle repaints the Canvases)
    restyle() {
        this._glyph.style = `color: ${rgba(Color.accent1_100)};`;
        this._label.setColour(Color.onWall);
    }

    // @state and @kind: finger.js's state and the bouncer's kind; an empty text hides the hint
    setState(state, kind = 'pin') {
        const text = fingerHint(state, kind);
        this.visible = !!text;
        if (!text)
            return;
        this._label.setText(text);
        const right = SCREEN_W - HINT.right;
        this._glyph.set_position(px(right - HINT.glyph), px(HINT.centreY - HINT.gap / 2 - HINT.glyph));
        this._label.set_position(px(right - this._label.textWidth),
            px(HINT.centreY + HINT.gap / 2 - this._label.inset - this._label.ink.y));
    }
});

// The way to the secret while an app's window shows over the lock screen: the lock screen's own child, so it takes the
// touch the lock screen's modal grab holds (keyguard.js adds it). The hint above it says the sensor is not the way in
// (after a restart, in Lockdown, past the misses); the button is. Back from the bouncer leaves the window over the lock
// screen again, and the button with it.
export const PinButton = GObject.registerClass(
class PinButton extends St.Widget {
    // @onTap: called for a click or a touch that ends on the button (Clutter.ClickAction: a drag off it is no tap)
    constructor(onTap) {
        super({reactive: true, visible: false});
        this._text = '';
        this._label = new AndroidText('', BUTTON.size, BUTTON.wght, {colour: Color.onWall, opsz: 16});
        this.add_child(this._label);
        const click = new Clutter.ClickAction();
        click.connect('clicked', () => onTap());
        this.add_action(click);
        this.restyle();
    }

    // the look is live, as the hint's: a tint of the theme's surface behind the secret's word, on the wallpaper's ink
    restyle() {
        this.style = `background-color: ${rgba(Color.surface, BUTTON.tint)}; border-radius: ${px(BUTTON.radius)}px;`;
        this._label.setColour(Color.onWall);
    }

    // @kind: the bouncer's kind ('pin' or 'pattern'): the word names the secret, and the button takes its width
    setKind(kind) {
        const text = secretWord(kind);
        if (text === this._text)
            return;
        this._text = text;
        this._label.setText(text);
        const width = this._label.textWidth + 2 * BUTTON.pad;
        this.width = px(width);
        this.height = px(BUTTON.height);
        this.set_position(px(SCREEN_W - BUTTON.right - width), px(BUTTON.top));
        this._label.set_position(px(BUTTON.pad - this._label.inset),
            px(BUTTON.inkTop - this._label.inset - this._label.ink.y));
    }
});

// fprintd's device, for its VerifyStatus signals. The system bus lets anyone read them (its policy allows every
// user to talk to the service): a failure here only costs the words, never the unlock.
export class FingerSensor {
    // @onStatus: (state) with the finger.js state a VerifyStatus names
    constructor(onStatus) {
        this._onStatus = onStatus;
        this._asked = false;
        this._subId = 0;
    }

    start() {
        if (this._asked)
            return;
        this._asked = true;
        Gio.DBus.system.call('net.reactivated.Fprint', '/net/reactivated/Fprint/Manager',
            'net.reactivated.Fprint.Manager', 'GetDefaultDevice', null, Gio.DBusCallFlags.NONE, -1, null,
            (bus, res) => {
                try {
                    const [path] = bus.call_finish(res).deepUnpack();
                    // stopped while the answer was on its way: no subscription to leave behind
                    if (this._asked)
                        this._subscribe(path);
                } catch (e) {
                    // fprintd is not on the bus yet: the run itself activates it, and the next start() tries again
                    this._asked = false;
                }
            });
    }

    _subscribe(path) {
        if (this._subId)
            return;
        this._subId = Gio.DBus.system.signal_subscribe('net.reactivated.Fprint', 'net.reactivated.Fprint.Device',
            'VerifyStatus', path, null, Gio.DBusSignalFlags.NONE,
            (bus, sender, object, iface, signal, params) => {
                const state = VERIFY_STATES[params.deepUnpack()[0]];
                if (state)
                    this._onStatus(state);
            });
    }

    stop() {
        if (this._subId)
            Gio.DBus.system.signal_unsubscribe(this._subId);
        this._subId = 0;
        this._asked = false;
    }
}
