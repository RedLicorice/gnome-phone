// The touch keyboard's keys: Hacker's Keyboard's full layout and behaviour (LatinKeyboardView, PointerTracker) in one
// actor. Every touch is handled here, not by a widget per key: a key highlights, ticks and previews on the finger's
// down, and types on its up, or as soon as another finger comes down (rollover). A finger slides between keys (with
// hysteresis), a long press opens the key's alternates or repeats it, and a fast swipe over the keyboard runs the
// swipe actions. Shift, Ctrl, Alt and Meta are sticky: a tap holds them for the next key, a second tap locks them, a
// finger kept on one applies it to the keys typed meanwhile (a chord).
//
// Over the keys sits a bar: the layout switch (full or compact), and the input method's word suggestions while a word
// is typed, Hacker's controls (Esc, Tab, Ctrl, Alt, the arrows, emoji, settings, hide) otherwise. Its keys are keys
// like the others, so they tick, repeat and stick the same way. The compact layout has three pages: letters, digits
// with the common punctuation, and the other symbols.
//
// What the keys type goes out through the shell keyboard's own controller (KeyboardController: the input method, or
// the virtual keyboard while a modifier is held), one item after the other.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as Haptics from '../haptics.js';
import {BAR_CONTROLS, CURSOR, ICONS, KEYVALS, LABELS, MODIFIERS} from './layout.js';

const SWIPE_MAX_MS = 400; // a swipe is fast; a slower move is a finger sliding between keys
const OFF = 0, ONCE = 1, LOCKED = 2;

export const KeyboardView = GObject.registerClass({
    Signals: {'action': {param_types: [GObject.TYPE_STRING]}},
}, class KeyboardView extends St.Widget {
    // @data a language's full layouts (layout.js load()); @compact its compact pages (compactFor());
    // @settings the keyboard's Gio.Settings
    _init(data, compact, settings) {
        super._init({style_class: 'hk-view', reactive: true, x_expand: true, y_expand: true});
        this._data = data;
        this._compact = compact;
        this._page = 'letters';
        this._suggestions = []; // {text, callback}, the input method's candidates in its order
        this._suggestionsVisible = false;
        this._barId = 0;
        this._barPending = false;
        this._units = 1;
        this._settings = settings;
        this.controller = null; // KeyboardController, set by the keyboard that shows this view
        this.keyboard = null; // the shell's Keyboard
        // the stock Keyboard reads these off its current page (_setCurrentLevelLatched, _setActiveLevel)
        this.shiftKeys = [];

        this._fn = false;
        this._select = false;
        this._mods = {shift: OFF, ctrl: OFF, alt: OFF, meta: OFF};
        this._held = new Map(); // modifier code -> {chorded}
        this._touches = new Map(); // sequence slot -> touch state
        this._out = Promise.resolve();
        this._keys = [];

        this._previewLabel = new St.Label({x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER});
        this._preview = new St.Bin({style_class: 'hk-preview', visible: false, child: this._previewLabel});
        this._popup = new St.BoxLayout({style_class: 'hk-popup', visible: false});
        // over the keyboard and the windows, untracked: they take no input and change no input region
        for (const actor of [this._preview, this._popup])
            Main.layoutManager.uiGroup.add_child(actor);

        this.connect('touch-event', (_a, event) => this._onEvent(event));
        this.connect('button-press-event', (_a, event) => this._onEvent(event));
        this.connect('button-release-event', (_a, event) => this._onEvent(event));
        this.connect('motion-event', (_a, event) => this._onEvent(event));
        this.connect('notify::mapped', () => {
            if (this.mapped)
                return;
            this._cancelAll();
            // the next field starts on the letters
            if (this._page !== 'letters') {
                this._page = 'letters';
                this._build();
            }
        });
        this.connect('destroy', () => this._onDestroy());

        this._build();
    }

    // ---- layout ----

    _compactMode() {
        return this._settings.get_string('mode') === 'compact';
    }

    _rows() {
        if (this._compactMode())
            return this._compact[this._page];
        const layout = this._data.layouts[this._fn ? 'full_fn' : 'full'];
        const extension = this._settings.get_boolean('extension-row');
        return layout.filter(row => extension || !row.extension);
    }

    // the keyboard's height in rows (the bar and the top rows may be shorter), for the stylesheet's label size
    rowUnits() {
        return this._units;
    }

    _build() {
        this._cancelAll();
        for (const key of this._keys)
            key.actor.destroy();
        this._keys = [];
        // the full layout's extension row and number row take the top row scale
        const scale = this._compactMode() ? 1 : this._settings.get_int('top-row-scale') / 100;
        this._barUnits = this._settings.get_boolean('bar') ? this._settings.get_int('bar-scale') / 100 : 0;
        const rows = this._rows();
        let y = this._barUnits;
        rows.forEach((row, r) => {
            const h = (rows[0].extension ? r < 2 : r === 0) ? scale : 1;
            let x = 0;
            for (const def of row.keys) {
                if (!def.spacer)
                    this._keys.push(this._makeKey(def, x, y, def.width, h));
                x += def.width;
            }
            y += h;
        });
        this._units = y;
        this._buildBar();
    }

    // the bar's keys: the layout switch, then the suggestions (the best one in the middle) or the controls
    _buildBar() {
        for (const key of this._keys.filter(k => k.bar))
            key.actor.destroy();
        this._keys = this._keys.filter(k => !k.bar);
        if (this._barUnits) {
            const words = this._suggestionsVisible ? this._suggestions.slice(0, 3) : [];
            const defs = words.length
                ? (words.length === 3 ? [words[1], words[0], words[2]] : words).map(w =>
                    ({code: 'suggestion', label: w.text, callback: w.callback}))
                : BAR_CONTROLS;
            const width = 0.9 / defs.length;
            this._keys.push(this._makeKey({code: 'mode'}, 0, 0, 0.1, this._barUnits, true));
            defs.forEach((def, i) =>
                this._keys.push(this._makeKey(def, 0.1 + i * width, 0, width, this._barUnits, true)));
        }
        this._relabel();
        this.queue_relayout();
    }

    // the bar again after the suggestions changed, once they are all in; a finger on the bar keeps it until it lifts
    _queueBar() {
        if (this._barId)
            return;
        this._barId = GLib.idle_add_once(GLib.PRIORITY_DEFAULT, () => {
            this._barId = 0;
            if ([...this._touches.values()].some(t => t.key.bar))
                this._barPending = true;
            else
                this._buildBar();
        });
    }

    _barReleased() {
        if (this._barPending && ![...this._touches.values()].some(t => t.key.bar)) {
            this._barPending = false;
            this._buildBar();
        }
    }

    // the input method's word suggestions (Keyboard.addSuggestion and the others, as ibusCandidatePopup calls them)
    resetSuggestions() {
        this._suggestions = [];
        this._queueBar();
    }

    addSuggestion(text, callback) {
        this._suggestions.push({text, callback});
        this._suggestionsVisible = true;
        this._queueBar();
    }

    setSuggestionsVisible(visible) {
        this._suggestionsVisible = visible;
        this._queueBar();
    }

    _setPage(page) {
        this._page = page;
        this._build();
    }

    _makeKey(def, x, y, w, h, bar = false) {
        const code = def.code;
        const mod = !!code && code !== 'space';
        const classes = ['hk-key'];
        if (mod)
            classes.push('hk-mod');
        if (code === 'space')
            classes.push('hk-space');
        if (bar)
            classes.push(code === 'suggestion' ? 'hk-bar hk-suggestion' : 'hk-bar');
        const actor = new St.Bin({style_class: classes.join(' '), reactive: false});
        const icon = ICONS[code];
        let label = null;
        if (icon) {
            actor.set_child(new St.Icon({icon_name: icon}));
        } else {
            label = new St.Label({x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER});
            actor.set_child(label);
        }
        this.add_child(actor);
        // Hacker's Keyboard's popup lists lowercase alternates; shifted, they go upper case
        return {def, code, actor, label, mod, bar, rect: {x, y, w, h}, box: null};
    }

    _shifted() {
        return this._mods.shift !== OFF || this._held.has('shift');
    }

    _textFor(key) {
        const {def, code} = key;
        if (code === 'space')
            return this._data.locale === 'it' ? 'Italiano' : 'English';
        if (code === 'mode')
            return '';
        if (code)
            return def.label ?? LABELS[code] ?? code;
        const label = def.label ?? '';
        return this._shifted() ? def.shift ?? label.toUpperCase() : label;
    }

    // labels and modifier states after a shift, modifier or layer change
    _relabel() {
        for (const key of this._keys) {
            if (key.label)
                key.label.text = this._textFor(key);
            const state = key.code in this._mods ? this._mods[key.code]
                : key.code === 'fn' ? this._fn && ONCE
                    : key.code === 'select' ? this._select && ONCE : OFF;
            const held = this._held.has(key.code);
            if (key.code === 'shift' && key.actor.child instanceof St.Icon) {
                key.actor.child.icon_name = this._mods.shift === LOCKED
                    ? 'osk-caps-lock-symbolic' : 'osk-shift-symbolic';
            }
            key.actor[state === ONCE || held ? 'add_style_pseudo_class' : 'remove_style_pseudo_class']('checked');
            key.actor[state === LOCKED ? 'add_style_class_name' : 'remove_style_class_name']('hk-locked');
        }
    }

    vfunc_get_preferred_width(_forHeight) {
        return [0, 0];
    }

    vfunc_get_preferred_height(_forWidth) {
        return [0, 0];
    }

    vfunc_allocate(box) {
        this.set_allocation(box);
        const width = box.get_width();
        const height = Math.max(1, box.get_height() - this._settings.get_int('bottom-padding'));
        const unit = height / this._units;
        const gap = this._settings.get_int('key-gap') / 2;
        for (const key of this._keys) {
            const {x, y, w, h} = key.rect;
            key.box = {x1: x * width, y1: y * unit, x2: (x + w) * width, y2: (y + h) * unit};
            const child = new Clutter.ActorBox({
                x1: Math.round(key.box.x1 + gap), y1: Math.round(key.box.y1 + gap),
                x2: Math.round(key.box.x2 - gap), y2: Math.round(key.box.y2 - gap),
            });
            key.actor.allocate(child);
        }
    }

    // ---- touch ----

    // the key under a point, among the bar's keys or the others (@bar): a touch stays on the part it began on
    _keyAt(x, y, current, bar) {
        const hyst = this._settings.get_int('hysteresis');
        if (current?.box) {
            const b = current.box;
            if (x >= b.x1 - hyst && x < b.x2 + hyst && y >= b.y1 - hyst && y < b.y2 + hyst)
                return current;
        }
        let best = null, bestDist = Infinity;
        for (const key of this._keys) {
            const b = key.box;
            if (!b || key.bar !== bar)
                continue;
            if (x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2)
                return key;
            // off the keys (below the bottom padding, outside the width): the nearest one
            const dx = Math.max(b.x1 - x, 0, x - b.x2), dy = Math.max(b.y1 - y, 0, y - b.y2);
            const dist = dx * dx + dy * dy;
            if (dist < bestDist) {
                best = key;
                bestDist = dist;
            }
        }
        return best;
    }

    _onEvent(event) {
        const type = event.type();
        // a touchscreen's emulated pointer events come along with its touch events
        if (type !== Clutter.EventType.TOUCH_BEGIN && type !== Clutter.EventType.TOUCH_UPDATE &&
            type !== Clutter.EventType.TOUCH_END && type !== Clutter.EventType.TOUCH_CANCEL &&
            (event.get_flags() & Clutter.EventFlags.FLAG_POINTER_EMULATED))
            return Clutter.EVENT_PROPAGATE;
        const sequence = event.get_event_sequence();
        const slot = sequence ? sequence.get_slot?.() ?? String(sequence) : 'pointer';
        const [sx, sy] = event.get_coords();
        const [, x, y] = this.transform_stage_point(sx, sy);
        const time = event.get_time();
        switch (type) {
        case Clutter.EventType.TOUCH_BEGIN:
        case Clutter.EventType.BUTTON_PRESS:
            this._down(slot, x, y, time);
            break;
        case Clutter.EventType.TOUCH_UPDATE:
        case Clutter.EventType.MOTION:
            this._move(slot, x, y);
            break;
        case Clutter.EventType.TOUCH_END:
        case Clutter.EventType.BUTTON_RELEASE:
            this._up(slot, x, y, time);
            break;
        case Clutter.EventType.TOUCH_CANCEL:
            this._cancel(slot);
            break;
        }
        return Clutter.EVENT_STOP;
    }

    _down(slot, x, y, time) {
        if (this._touches.has(slot))
            this._cancel(slot);
        // rollover: a new finger types what the others are on (their up then does nothing)
        for (const other of this._touches.values()) {
            if (!other.done && !other.key.mod && !other.popup && !other.cursor && !other.repeated) {
                this._type(other.key);
                this._release(other);
                other.done = true;
            }
        }
        const bar = !!this._barUnits && this._keys.some(k => k.bar && k.box && y < k.box.y2);
        const offset = bar ? 0 : this._settings.get_int('touch-offset');
        const key = this._keyAt(x, y - offset, null, bar);
        if (!key)
            return;
        const touch = {key, bar, x0: x, y0: y, t0: time, done: false, repeated: false, popup: null, cursor: null};
        this._touches.set(slot, touch);
        if (key.code in MODIFIERS) {
            this._held.set(key.code, {chorded: false, before: this._mods[key.code]});
            this._relabel();
        }
        this._press(touch);
    }

    _move(slot, x, y) {
        const touch = this._touches.get(slot);
        if (!touch || touch.done)
            return;
        if (touch.popup) {
            this._popupSelect(touch, x);
            return;
        }
        // the space bar slides the cursor
        if (touch.key.code === 'space' && this._settings.get_boolean('space-cursor')) {
            const step = this.width / 14 / 2;
            if (!touch.cursor && Math.abs(x - touch.x0) > step) {
                touch.cursor = {anchor: touch.x0};
                this._release(touch);
            }
            if (touch.cursor) {
                let n = Math.trunc((x - touch.cursor.anchor) / step);
                touch.cursor.anchor += n * step;
                for (; n !== 0; n -= Math.sign(n))
                    this._sendKey(n > 0 ? 'right' : 'left');
                return;
            }
        }
        const offset = touch.bar ? 0 : this._settings.get_int('touch-offset');
        const key = this._keyAt(x, y - offset, touch.key, touch.bar);
        if (key && key !== touch.key && !(touch.key.code in MODIFIERS)) {
            this._release(touch);
            touch.key = key;
            touch.repeated = false;
            this._press(touch, false);
        }
    }

    _up(slot, x, y, time) {
        const touch = this._touches.get(slot);
        if (!touch)
            return;
        this._touches.delete(slot);
        const {key} = touch;
        this._release(touch);
        if (touch.bar) {
            if (!touch.done && key.code in MODIFIERS)
                this._endModifier(key, false);
            else if (!touch.done && !touch.repeated)
                this._type(key);
            this._barReleased();
            return;
        }
        if (touch.done)
            return;
        if (touch.popup) {
            const chosen = touch.popup.chosen;
            this._hidePopup(touch);
            if (chosen)
                this._typeText(chosen);
            return;
        }
        if (touch.cursor)
            return;
        if (this._swipe(touch, x, y, time))
            return this._endModifier(key, true);
        if (key.code in MODIFIERS)
            return this._endModifier(key, false);
        if (touch.repeated) {
            if (key.code === 'backspace' && !this._chording())
                this.controller?.toggleDelete(false);
            return;
        }
        this._type(key);
    }

    _cancel(slot) {
        const touch = this._touches.get(slot);
        if (!touch)
            return;
        this._touches.delete(slot);
        this._release(touch);
        if (touch.popup)
            this._hidePopup(touch);
        if (touch.repeated && touch.key.code === 'backspace')
            this.controller?.toggleDelete(false);
        if (touch.key.code in MODIFIERS) {
            this._held.delete(touch.key.code);
            this._relabel();
        }
        if (touch.bar)
            this._barReleased();
    }

    _cancelAll() {
        for (const slot of [...this._touches.keys()])
            this._cancel(slot);
    }

    // a swipe over the keyboard: its action instead of the key
    _swipe(touch, x, y, time) {
        if (time - touch.t0 > SWIPE_MAX_MS)
            return false;
        const dx = x - touch.x0, dy = y - touch.y0;
        const row = this.height / this._units;
        let direction = null;
        if (Math.abs(dy) > row * 1.6 && Math.abs(dy) > 2 * Math.abs(dx))
            direction = dy < 0 ? 'up' : 'down';
        else if (Math.abs(dx) > this.width * 0.3 && Math.abs(dx) > 2 * Math.abs(dy))
            direction = dx < 0 ? 'left' : 'right';
        if (!direction)
            return false;
        const action = this._settings.get_string(`swipe-${direction}`);
        if (action === 'none')
            return false;
        this.emit('action', action);
        return true;
    }

    // the finger's down on a key: highlight, tick, preview, and the long press
    _press(touch, tick = true) {
        const {key} = touch;
        key.actor.add_style_pseudo_class('active');
        if (tick && this._settings.get_boolean('haptics'))
            Haptics.vibrateRaw([[0.8, this._settings.get_int('haptic-ms')]]);
        if (!key.code && this._settings.get_boolean('key-preview'))
            this._showPreview(key);
        const repeat = key.def.repeat;
        const delay = this._settings.get_int(repeat ? 'repeat-delay-ms' : 'long-press-ms');
        touch.timer = GLib.timeout_add(GLib.PRIORITY_HIGH, delay, () => {
            touch.timer = 0;
            this._longPress(touch);
            return GLib.SOURCE_REMOVE;
        });
    }

    _release(touch) {
        if (touch.timer)
            GLib.source_remove(touch.timer);
        touch.timer = 0;
        if (touch.repeatTimer)
            GLib.source_remove(touch.repeatTimer);
        touch.repeatTimer = 0;
        touch.key.actor.remove_style_pseudo_class('active');
        this._preview.hide();
    }

    _longPress(touch) {
        const {key} = touch;
        if (key.def.repeat) {
            touch.repeated = true;
            if (key.code === 'backspace' && !this._chording()) {
                // the controller deletes on, word by word after a while, until it is told to stop
                this.controller?.toggleDelete(true);
                return;
            }
            this._type(key);
            touch.repeatTimer = GLib.timeout_add(GLib.PRIORITY_HIGH, this._settings.get_int('repeat-interval-ms'),
                () => {
                    this._type(key);
                    return GLib.SOURCE_CONTINUE;
                });
            return;
        }
        if (key.code === 'shift') {
            this._mods.shift = LOCKED;
            this._held.get('shift').chorded = true;
            this._syncLatched();
            this._relabel();
        } else if (key.code === 'menu') {
            touch.done = true;
            this.emit('action', 'settings');
        } else if (key.code === 'space') {
            touch.done = true;
            this.emit('action', 'lang-next');
        } else if (key.def.popup) {
            this._preview.hide();
            this._showPopup(touch);
        }
    }

    // ---- preview and alternates ----

    _showPreview(key) {
        const [kx, ky] = key.actor.get_transformed_position();
        const [kw, kh] = key.actor.get_transformed_size();
        this._previewLabel.text = this._textFor(key);
        const w = Math.max(kw * 1.4, kh), h = kh * 1.3;
        this._preview.set_size(w, h);
        this._preview.set_position(Math.round(kx + (kw - w) / 2), Math.round(ky - h - 4));
        this._preview.show();
    }

    _showPopup(touch) {
        const {key} = touch;
        const chars = key.def.popup.map(c => this._shifted() ? c.toUpperCase() : c);
        this._popup.destroy_all_children();
        const [kx, ky] = key.actor.get_transformed_position();
        const [kw, kh] = key.actor.get_transformed_size();
        const items = chars.map(c => {
            const item = new St.Bin({
                style_class: 'hk-popup-item', width: kw, height: kh,
                child: new St.Label({text: c, x_align: Clutter.ActorAlign.CENTER, y_align: Clutter.ActorAlign.CENTER}),
            });
            this._popup.add_child(item);
            return item;
        });
        const [, natWidth] = this._popup.get_preferred_width(-1);
        const monitor = Main.layoutManager.keyboardMonitor ?? Main.layoutManager.primaryMonitor;
        const x = Math.clamp(kx, monitor.x, monitor.x + monitor.width - natWidth);
        this._popup.set_position(Math.round(x), Math.round(ky - kh - 12));
        this._popup.show();
        touch.popup = {items, chars, chosen: null};
        this._popupSelect(touch, touch.x0);
    }

    _popupSelect(touch, x) {
        const {items, chars} = touch.popup;
        const [px] = this.get_transformed_position();
        let chosen = -1;
        items.forEach((item, i) => {
            const [ix] = item.get_transformed_position();
            const inside = px + x >= ix && px + x < ix + item.width;
            if (inside || (i === 0 && px + x < ix) || (i === items.length - 1 && px + x >= ix + item.width))
                chosen = i;
        });
        items.forEach((item, i) =>
            item[i === chosen ? 'add_style_pseudo_class' : 'remove_style_pseudo_class']('selected'));
        touch.popup.chosen = chars[chosen] ?? null;
    }

    _hidePopup(touch) {
        this._popup.hide();
        this._popup.destroy_all_children();
        touch.popup = null;
    }

    // ---- modifiers ----

    _chording() {
        return ['ctrl', 'alt', 'meta'].some(m => this._mods[m] !== OFF || this._held.has(m));
    }

    // a modifier's up: a tap steps it (off, once, locked, off); after a chord it goes back to what it was
    _endModifier(key, cancelled) {
        if (!(key.code in MODIFIERS)) {
            if (key.code === 'fn' && !cancelled)
                this._toggleFn();
            return;
        }
        const held = this._held.get(key.code);
        this._held.delete(key.code);
        if (!cancelled && held && !held.chorded)
            this._mods[key.code] = (this._mods[key.code] + 1) % 3;
        else if (held?.chorded && key.code !== 'shift')
            this._mods[key.code] = held.before === LOCKED ? LOCKED : OFF;
        this._syncLatched();
        this._relabel();
    }

    // the stock keyboard leaves the level alone while latched (_updateLevelFromHints): caps lock is a latch
    _syncLatched() {
        if (this.keyboard)
            this.keyboard._latched = this._mods.shift === LOCKED;
    }

    // what the stock keyboard asks for from the content hints: shift for a sentence's first letter, or not
    setAutoShift(shift) {
        if (this._mods.shift === LOCKED || this._held.has('shift'))
            return;
        const state = shift ? ONCE : OFF;
        if (this._mods.shift === state)
            return;
        this._mods.shift = state;
        this._relabel();
    }

    _toggleFn() {
        this._fn = !this._fn;
        this._build();
    }

    rebuild() {
        this._build();
    }

    // the modifiers to send with the next key, and the one-shot ones dropped after it
    _takeModifiers(withShift) {
        const keyvals = new Set();
        for (const [name, keyval] of Object.entries(MODIFIERS)) {
            if (name === 'shift' && !withShift)
                continue;
            if (this._mods[name] !== OFF || this._held.has(name))
                keyvals.add(keyval);
        }
        for (const name of this._held.keys()) {
            const held = this._held.get(name);
            if (held)
                held.chorded = true;
        }
        let changed = false;
        for (const name of Object.keys(this._mods)) {
            if (this._mods[name] === ONCE && !this._held.has(name)) {
                this._mods[name] = OFF;
                changed = true;
            }
        }
        if (changed)
            this._relabel();
        return keyvals;
    }

    // ---- output ----

    _queue(fn) {
        this._out = this._out.then(fn).catch(e => logError(e, 'gnome-phone: touch keyboard'));
    }

    _type(key) {
        const {code} = key;
        if (!code)
            return this._typeText(this._textFor(key), key.def.label);
        switch (code) {
        case 'space':
            return this._typeText(' ', ' ');
        case 'fn':
            return this._toggleFn();
        case 'select':
            this._select = !this._select;
            return this._relabel();
        case 'menu':
        case 'emoji':
            return this.emit('action', 'emoji');
        case 'hide':
            return this.emit('action', 'close');
        case 'mode':
            return this._settings.set_string('mode', this._compactMode() ? 'full' : 'compact');
        case 'sym':
        case 'sym2':
            return this._setPage(code);
        case 'abc':
            return this._setPage('letters');
        case 'suggestion':
            this._takeModifiers(false);
            return this._queue(() => key.def.callback?.());
        case 'settings':
            return this.emit('action', 'settings');
        case 'backspace':
            if (!this._chording()) {
                this._takeModifiers(false);
                return this._queue(() => {
                    this.controller?.toggleDelete(true);
                    this.controller?.toggleDelete(false);
                    this.keyboard?._updateLevelFromHints(true);
                });
            }
            return this._sendKey(code);
        default:
            return this._sendKey(code);
        }
    }

    // a character: as text through the input method, or as a key with the modifiers held (Ctrl+Shift+T sends
    // Shift and the unshifted key, as a hardware keyboard would)
    _typeText(text, base = text) {
        const chord = this._chording();
        const modifiers = this._takeModifiers(chord);
        const controller = this.controller;
        if (!controller)
            return;
        this._queue(async () => {
            await controller.commit(chord ? base : text, chord ? modifiers : null);
            this.keyboard?._updateLevelFromHints(true);
        });
    }

    // a special key (arrows, Tab, Esc, F-keys...) with the modifiers held, and Shift in selection mode
    _sendKey(code) {
        const keyval = KEYVALS[code];
        const controller = this.controller;
        if (!keyval || !controller)
            return;
        const modifiers = this._takeModifiers(true);
        if (this._select && CURSOR.has(code))
            modifiers.add(MODIFIERS.shift);
        this._queue(() => {
            for (const m of modifiers)
                controller.keyvalPress(m);
            controller.keyvalPress(keyval);
            controller.keyvalRelease(keyval);
            for (const m of [...modifiers].reverse())
                controller.keyvalRelease(m);
        });
    }

    _onDestroy() {
        if (this._barId)
            GLib.source_remove(this._barId);
        this._barId = 0;
        this._cancelAll();
        this._preview.destroy();
        this._popup.destroy();
    }
});
