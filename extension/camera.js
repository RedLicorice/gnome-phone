// The camera indicator. Whenever a camera is open a chip shows in the status bar (statusbar.js paints it, over the lock
// screen too: the lock screen draws the same system icons), and a tap on it opens a sheet: who has the camera and why,
// what Minerva's captures recognised (names with scores, a thumbnail with the faces boxed) and the last captures.
//
// The facts come from minerva-camwatch over D-Bus (org.minerva.Camera1, eyes/minerva_eyes/camwatch.py): it finds any
// app's camera by its powered sensor and takes Minerva's own reports. Nothing here polls: the service's Changed
// signal and its appearing or vanishing on the bus are all that move this. Frames stay on the phone: a thumbnail is a
// small file in $XDG_RUNTIME_DIR that the service writes and deletes.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {clock, doing, facesText, historyLine, owner, parseState} from './cameratext.js';
import {animate, bezier} from './motion.js';
import {Color, cogl, font, gild, restyleActor, rgba} from './palette.js';
import {px, SCREEN_H, SCREEN_W} from './units.js';

const BUS = 'org.minerva.Camera1';
const PATH = '/org/minerva/Camera1';

// the sheet, screen px (1 dp = 3 px)
const MARGIN = 72;
const PAD = 54;
const CORNER = 84;
const GAP = 24;
const THUMB = 330;
const TITLE_SP = 20;
const LINE_SP = 15;
const SMALL_SP = 13;
const ENTER_MS = 220;
const EXIT_MS = 150;
const DIM = 0.6;
const HISTORY_MAX = 5;
const STANDARD = bezier(0.2, 0, 0, 1);
const STANDARD_ACCELERATE = bezier(0.3, 0, 1, 1);

// The state of the cameras: `active` (one entry per camera in use) and `history`, as camwatch publishes them. 'changed'
// when either moved; 'tapped' when the chip was tapped.
export const CameraWatch = GObject.registerClass({
    Signals: {'changed': {}, 'tapped': {}},
}, class CameraWatch extends GObject.Object {
    constructor() {
        super();
        this.active = [];
        this.history = [];
        this._text = '';
        this._cancellable = new Gio.Cancellable();
        this._subscription = Gio.DBus.session.signal_subscribe(BUS, BUS, 'Changed', PATH, null,
            Gio.DBusSignalFlags.NONE, (c, sender, path, iface, signal, params) => this._set(params.deepUnpack()[0]));
        this._watch = Gio.bus_watch_name(Gio.BusType.SESSION, BUS, Gio.BusNameWatcherFlags.NONE,
            () => this._fetch(), () => this._set(''));
    }

    _fetch() {
        Gio.DBus.session.call(BUS, PATH, BUS, 'GetState', null, new GLib.VariantType('(s)'),
            Gio.DBusCallFlags.NO_AUTO_START, 2000, this._cancellable, (conn, res) => {
                try {
                    this._set(conn.call_finish(res).deepUnpack()[0]);
                } catch (e) {
                    if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        log(`gnome-phone: camera state: ${e.message}`);
                }
            });
    }

    _set(text) {
        if (text === this._text)
            return;
        this._text = text;
        const state = parseState(text);
        this.active = state.active;
        this.history = state.history;
        this.emit('changed');
    }

    get inUse() {
        return this.active.length > 0;
    }

    tap() {
        this.emit('tapped');
    }

    destroy() {
        this._cancellable.cancel();
        Gio.DBus.session.signal_unsubscribe(this._subscription);
        Gio.bus_unwatch_name(this._watch);
    }
});

function label(text, sp, weight, colour, wrap = true) {
    const l = new St.Label({text, style: `color: ${rgba(colour, 1)};`});
    l.clutter_text.font_name = font(sp, weight);
    l.clutter_text.line_wrap = wrap;
    l.clutter_text.ellipsize = 0;
    return l;
}

// The sheet a tap on the chip opens: over everything (the lock screen too), a dim a tap on closes, rebuilt as the
// state changes while it is up
export class CameraSheet {
    _panelStyle() {
        return `background-color: ${rgba(Color.surfaceContainerLow)}; border-radius: ${px(CORNER)}px; ` +
            `padding: ${px(PAD)}px;${gild(0.5, true)}`;
    }

    constructor(watch) {
        this._watch = watch;
        this._grab = null;
        this._anim = null;
        this._closing = false;
        this.layer = new St.Widget({width: px(SCREEN_W), height: px(SCREEN_H), visible: false});
        this._scrim = new St.Button({width: px(SCREEN_W), height: px(SCREEN_H), button_mask: St.ButtonMask.PRIMARY});
        this._scrim.connect('clicked', () => this.close());
        this.layer.add_child(this._scrim);
        this._panel = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL, reactive: true, x: px(MARGIN), width: px(SCREEN_W - 2 * MARGIN),
            style: this._panelStyle(),
        });
        this.layer.add_child(this._panel);
        // the look is live: the panel's style, and the labels are built again if it is up
        restyleActor(this._panel, () => {
            this._panel.style = this._panelStyle();
            if (this.layer.visible)
                this._rebuild();
        });
        this._ids = [watch.connect('tapped', () => this.open()),
            watch.connect('changed', () => this.layer.visible && this._rebuild())];
    }

    get showing() {
        return this.layer.visible && !this._closing;
    }

    open() {
        if (this.layer.visible)
            return;
        this.layer.show();
        this._rebuild();
        this._grab = Main.pushModal(this.layer, {actionMode: Shell.ActionMode.SYSTEM_MODAL});
        this._animate(true);
    }

    close() {
        if (!this.layer.visible || this._closing)
            return;
        this._closing = true;
        this._animate(false, () => this._hide());
    }

    closeNow() {
        if (!this.layer.visible)
            return;
        this._anim?.stop();
        this._hide();
    }

    _hide() {
        this._anim = null;
        this._closing = false;
        this.layer.hide();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this._panel.destroy_all_children();
    }

    _animate(enter, onDone = null) {
        this._anim?.stop();
        this._anim = animate(this.layer, {
            from: 0, to: 1,
            duration: enter ? ENTER_MS : EXIT_MS,
            interpolator: enter ? STANDARD : STANDARD_ACCELERATE,
            onUpdate: p => {
                const alpha = enter ? p : 1 - p;
                this._panel.opacity = Math.round(alpha * 255);
                this.layer.background_color = cogl(Color.dim, DIM * alpha);
            },
            onDone,
        });
    }

    _rebuild() {
        const panel = this._panel;
        panel.destroy_all_children();
        const {active, history} = this._watch;
        panel.add_child(label(active.length ? 'Fotocamera in uso' : 'Fotocamera non in uso', TITLE_SP, 700, Color.onSurface));
        for (const entry of active)
            panel.add_child(this._card(entry));
        const past = history.slice(0, HISTORY_MAX);
        if (past.length) {
            const heading = label('Ultime riprese', LINE_SP, 700, Color.onSurface);
            heading.style += ` margin-top: ${px(GAP)}px;`;
            panel.add_child(heading);
            for (const entry of past) {
                const line = label(historyLine(entry), SMALL_SP, 500, Color.onSurfaceVariant);
                line.style += ` margin-top: ${px(GAP / 3)}px;`;
                panel.add_child(line);
            }
        }
        // the picture of the latest capture that kept one: what the faces were boxed on
        const shot = history.find(e => e.thumb);
        if (shot && !active.some(e => e.thumb)) {
            const caption = label(`Ultima immagine, ${clock(shot.end)}`, SMALL_SP, 500, Color.onSurfaceVariant);
            caption.style += ` margin-top: ${px(GAP)}px;`;
            panel.add_child(caption);
            panel.add_child(this._thumb(shot.thumb));
        }
        // centred on the screen, whatever its height
        const [, h] = panel.get_preferred_height(px(SCREEN_W - 2 * MARGIN));
        panel.set_position(px(MARGIN), Math.max(0, Math.round((px(SCREEN_H) - h) / 2)));
    }

    // one camera in use: who and why, which camera and since when, what it recognised, the last picture
    _card(entry) {
        const card = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL, style: `margin-top: ${px(GAP)}px;`});
        card.add_child(label(`${owner(entry)} · ${entry.label}`, LINE_SP, 700, Color.primary));
        card.add_child(label(doing(entry), LINE_SP, 500, Color.onSurface));
        card.add_child(label(`dalle ${clock(entry.since)}`, SMALL_SP, 500, Color.onSurfaceVariant));
        const faces = facesText(entry);
        if (faces)
            card.add_child(label(faces, LINE_SP, 500, Color.onSurface));
        if (entry.thumb)
            card.add_child(this._thumb(entry.thumb));
        return card;
    }

    _thumb(path) {
        return new St.Bin({
            style: `margin-top: ${px(GAP / 2)}px; width: ${px(THUMB)}px; height: ${px(THUMB)}px; ` +
                `background-image: url("file://${path}"); background-size: contain; background-repeat: no-repeat; ` +
                `background-position: left center;`,
            x_align: Clutter.ActorAlign.START,
        });
    }

    destroy() {
        this._ids.forEach(id => this._watch.disconnect(id));
        this.closeNow();
        this.layer.destroy();
    }
}
