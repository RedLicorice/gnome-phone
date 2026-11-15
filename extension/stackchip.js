// The agent chip: a small pill the shell draws over an agent's window while the user has it in front, so they know
// whose window it is and can take it over or hand it back. Only the shell draws it (an app cannot paint over the
// shell's own chrome), so an agent's window cannot pass for the system's own.
//
//   ● minerva            the agent's window: tap to take it over (the agent may no longer close or focus it)
//   ‖ minerva · resume   taken over: tap to hand it back
//
// Nothing animates and nothing runs while it is hidden.
import Clutter from 'gi://Clutter';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {Color, dataFont, font, restyleActor, rgba} from './palette.js';
import {owlIcon} from './owl.js';
import {isRenaissance} from './theme.js';
import {overWindows} from './scanout.js';
import {px} from './units.js';

// In the status bar's band (116 px high), after the clock and before the icons: over the window's own controls it
// would hide a gear or a back arrow.
const LEFT = 190;       // px from the screen's left, past the clock
const HEIGHT = 64;
const TOP = (116 - HEIGHT) / 2;
const PAD = 28;
const FONT_SP = 12;

const chipFont = () => dataFont(FONT_SP, 600) ?? font(FONT_SP, 600);
const nice = owner => owner.charAt(0).toUpperCase() + owner.slice(1);

export class StackChip {
    // onToggle(): the user tapped the chip
    constructor(onToggle) {
        this._onToggle = onToggle;
        this.rect = null;                       // [x0, y0, x1, y1] in screen pixels while shown
        this._button = new St.Button({
            name: 'gnomePhoneStackChip', reactive: true, track_hover: false, can_focus: false, visible: false,
            x_align: Clutter.ActorAlign.START, y_align: Clutter.ActorAlign.START,
        });
        this._label = new St.Label({y_align: Clutter.ActorAlign.CENTER});
        const text = this._label.clutter_text;
        text.font_name = chipFont();
        // in the Renaissance looks the owl alone stands for the agent (no dot)
        this._owl = owlIcon(px(HEIGHT * 0.7));
        this._owl.y_align = Clutter.ActorAlign.CENTER;
        this._owl.visible = isRenaissance();
        const box = new St.BoxLayout({style: `spacing: ${px(PAD / 2)}px;`});
        box.add_child(this._owl);
        box.add_child(this._label);
        this._button.set_child(box);
        this._owner = null;
        this._takenOver = false;
        // the look is live: the chip's colours, its face and its owl
        restyleActor(this._button, () => {
            text.font_name = chipFont();
            this._owl.visible = isRenaissance();
            if (this._button.visible)
                this._style();
        });
        this._button.connect('clicked', () => this._onToggle());
        Main.layoutManager.addTopChrome(this._button);
        overWindows(this._button);
    }

    // owner: the agent's name; takenOver: whether the user has the window
    show(owner, takenOver) {
        const m = Main.layoutManager.primaryMonitor;
        this._owner = owner;
        this._takenOver = takenOver;
        this._style();
        this._button.visible = true;
        this._button.set_position(m.x + px(LEFT), m.y + px(TOP));
        this._sync();
    }

    _style() {
        const owner = this._owner, takenOver = this._takenOver;
        const [fg, bg, edge] = takenOver
            ? [Color.onPrimary, rgba(Color.primary, 0.95), rgba(Color.primary, 1)]
            : [Color.onSurface, rgba(Color.surface, 0.88), rgba(Color.silver, 0.7)];
        this._label.text = takenOver ? `‖ ${nice(owner)} · resume` : `${isRenaissance() ? '' : '● '}${nice(owner)}`;
        this._button.style = `padding: 0 ${px(PAD)}px; min-height: ${px(HEIGHT)}px; border-radius: ${px(HEIGHT / 2)}px; ` +
            `background-color: ${bg}; border: ${Math.max(1, px(3))}px solid ${edge};`;
        this._label.style = `color: ${rgba(fg, 1)};`;
    }

    hide() {
        this._button.visible = false;
        this.rect = null;
    }

    _sync() {
        const [x, y] = this._button.get_transformed_position();
        const [w, h] = this._button.get_transformed_size();
        this.rect = [x, y, x + w, y + h];
    }

    get shown() {
        return this._button.visible;
    }

    destroy() {
        this._button.destroy();
        this._button = null;
    }
}
