// The power menu: Android 16's GlobalActionsDialogLite as LineageOS 23.2 builds it (GlobalActionsDialogLite.java,
// global_actions_grid_lite.xml, global_actions_grid_item_lite.xml). A black dim at 0.6 over everything, and a
// SurfaceContainerLow panel with 28 dp corners and 24 dp of padding holding two columns of 96 dp SurfaceContainerHighest
// circles 24 dp apart, each with its 20 dp icon and a 14 sp label 8 dp under it; a short last row is packed in the
// middle. It comes in over 220 ms (STANDARD), sliding 20 dp in from the power key's side as it fades in with the dim,
// and goes in 150 ms (STANDARD_ACCELERATE), 10 dp the other way. A pressed button shrinks to 0.97 in 50 ms and comes
// back in 250 (control_state_list_animator). A tap outside the panel closes it.
//
// The items are config_globalActionsList's that this phone can back: Lockdown, shown only with a PIN or a pattern
// set (the lock screen at once, its notifications kept off it until the next unlock), Power off and Restart
// (logind's: gnome-session 51 has only its dialog's Shutdown and Reboot, no RequestShutdown/RequestReboot any more).
// Emergency is left out: nothing here can place an emergency call. Between them, this phone's own: Android, a restart
// into the LineageOS it dual-boots with (the kernel's syscon-reboot-mode "android"; a plain restart comes back here).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {VectorIcon} from './icons.js';
import {animate, bezier, LINEAR} from './motion.js';
import {Color, cogl, gild, rgba} from './palette.js';
import {AndroidText} from './text.js';
import {px, SCREEN_H, SCREEN_W} from './units.js';

Gio._promisify(Gio.DBusConnection.prototype, 'call');

// screen px (1 dp = 3 px)
const PAD = 72;                 // global_actions_lite_padding: around the grid and between its cells
const BUTTON = 288;             // global_actions_button_size
const ICON = 60;                // the button less 2 × global_actions_button_padding (38 dp)
const LABEL_GAP = 24;           // global_actions_grid_container_bottom_margin
const CORNER = 84;              // global_actions_corner_radius
const COLUMNS = 2;              // the Flow's maxElementsWrap
const LABEL_SP = 14;
const LABEL_WGHT = 700;         // textAppearanceSmall's 400, and the phone's bold text (+300)
const DIM = 0.6;                // the dark dialog theme's backgroundDimAmount (Theme.Material)
const ENTER_MS = 220;           // config_activityDefaultDur
const EXIT_MS = 150;            // config_activityShortDur
const ENTER_DX = 60;            // popup_enter_animation_from_y_delta, along x with the power key on the right
const EXIT_DX = -30;            // popup_exit_animation_to_y_delta
const STANDARD = bezier(0.2, 0, 0, 1);
const STANDARD_ACCELERATE = bezier(0.3, 0, 1, 1);
const PRESSED_SCALE = 0.97;
const PRESS_MS = 50;
const RELEASE_MS = 250;

function shapeStyle(color, radius) {
    return `background-color: ${rgba(color)}; border-radius: ${px(radius).toFixed(2)}px;${gild(0.5)}`;
}

function login1(method, params) {
    return Gio.DBus.system.call('org.freedesktop.login1', '/org/freedesktop/login1',
        'org.freedesktop.login1.Manager', method, params, null, Gio.DBusCallFlags.NONE, -1, null);
}

// PowerOff or Reboot, a reboot with the kernel's reboot mode `mode` ('' for the default: this system again); the
// shell's session may (polkit says yes without asking)
async function logind(method, mode = '') {
    try {
        if (method === 'Reboot')
            await login1('SetRebootParameter', new GLib.Variant('(s)', [mode]));
        await login1(method, new GLib.Variant('(b)', [false]));
    } catch (e) {
        log(`gnome-phone: ${method}: ${e.message}`);
    }
}

export class PowerMenu {
    // secure(): a PIN or pattern is set; lockdown(): lock now
    constructor({secure, lockdown}) {
        this._secure = secure;
        this._lockdown = lockdown;
        this._closing = false;
        this._grab = null;
        this._anim = null;

        this.layer = new St.Widget({width: px(SCREEN_W), height: px(SCREEN_H), visible: false});
        // the dim, which a tap closes the menu on
        this._scrim = new St.Button({width: px(SCREEN_W), height: px(SCREEN_H), button_mask: St.ButtonMask.PRIMARY});
        this._scrim.connect('clicked', () => this.close());
        this.layer.add_child(this._scrim);
        this._panel = new St.Widget({reactive: true, style: shapeStyle(Color.surfaceContainerLow, CORNER)});
        this.layer.add_child(this._panel);
    }

    get showing() {
        return this.layer.visible && !this._closing;
    }

    open() {
        if (this.layer.visible)
            return;
        this._build();
        this.layer.show();
        this._grab = Main.pushModal(this.layer, {actionMode: Shell.ActionMode.SYSTEM_MODAL});
        this._animate(true);
    }

    // then: what the pressed item does, once the menu is gone
    close(then = null) {
        if (!this.layer.visible || this._closing)
            return;
        this._closing = true;
        this._animate(false, () => this._hide(then));
    }

    closeNow() {
        if (!this.layer.visible)
            return;
        this._anim?.stop();
        this._hide(null);
    }

    _hide(then) {
        this._anim = null;
        this._closing = false;
        this.layer.hide();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this._panel.destroy_all_children();
        then?.();
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
                this._panel.translation_x = px(enter ? ENTER_DX * (1 - p) : EXIT_DX * p);
                this.layer.background_color = cogl(Color.black, DIM * alpha);
            },
            onDone,
        });
    }

    _build() {
        const items = [];
        if (this._secure())
            items.push(['Lockdown', 'ic_lock_lockdown_round', () => this._lockdown()]);
        items.push(['Power off', 'ic_lock_power_off_round', () => logind('PowerOff')]);
        items.push(['Android', 'ic_android', () => logind('Reboot', 'android')]);
        items.push(['Restart', 'ic_restart_round', () => logind('Reboot')]);

        const labels = items.map(([label]) =>
            new AndroidText(label, LABEL_SP, LABEL_WGHT, {colour: Color.onSurface}));
        const cellH = BUTTON + LABEL_GAP + Math.max(...labels.map(l => l.textHeight));
        const columns = Math.min(items.length, COLUMNS);
        const rows = Math.ceil(items.length / COLUMNS);
        const w = PAD + columns * (BUTTON + PAD);
        const h = PAD + rows * (cellH + PAD);
        this._panel.set_position(px(Math.round((SCREEN_W - w) / 2)), px(Math.round((SCREEN_H - h) / 2)));
        this._panel.set_size(px(w), px(h));
        items.forEach(([label, icon, action], i) => {
            const row = Math.floor(i / COLUMNS);
            const inRow = Math.min(COLUMNS, items.length - row * COLUMNS);
            const rowX = (w - inRow * BUTTON - (inRow - 1) * PAD) / 2;
            const x = rowX + (i % COLUMNS) * (BUTTON + PAD);
            this._panel.add_child(this._item(label, labels[i], icon, action, x, PAD + row * (cellH + PAD), cellH));
        });
    }

    _item(label, text, icon, action, x, y, height) {
        const button = new St.Button({
            x: px(x), y: px(y), width: px(BUTTON), height: px(height),
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            accessible_name: label,
        });
        button.set_pivot_point(0.5, 0.5);
        const box = new St.Widget({width: px(BUTTON), height: px(height)});
        box.add_child(new St.Widget({width: px(BUTTON), height: px(BUTTON),
            style: shapeStyle(Color.surfaceContainerHighest, BUTTON / 2)}));
        const glyph = new VectorIcon(icon, px(ICON), Color.onSurface);
        glyph.set_position(px((BUTTON - ICON) / 2), px((BUTTON - ICON) / 2));
        box.add_child(glyph);
        text.set_position(px(BUTTON / 2 - text.textWidth / 2 - text.inset), px(BUTTON + LABEL_GAP - text.inset));
        box.add_child(text);
        button.set_child(box);

        let scale = null;
        button.connect('notify::pressed', () => {
            scale?.stop();
            scale = animate(button, {
                from: button.scale_x, to: button.pressed ? PRESSED_SCALE : 1,
                duration: button.pressed ? PRESS_MS : RELEASE_MS,
                interpolator: LINEAR,
                onUpdate: s => button.set_scale(s, s),
            });
        });
        button.connect('clicked', () => this.close(action));
        return button;
    }

    destroy() {
        this.closeNow();
        this.layer.destroy();
    }
}
