// Every window inside the screen. An app's window is maximized (stack.js), which makes it as big as the work area, but
// a client may not be made smaller than it says it can be: a desktop dialog (xdg-desktop-portal-gnome's "Remote
// Desktop", 540 px wide on a 360 px screen) keeps its own minimum, and its buttons end up beyond the edge where no
// finger reaches. Such a window is shown scaled down about its top left to the work area, centred in it: still the
// same window (mutter maps a touch on the scaled actor back to the surface), only smaller. A window that fits is
// left as it is, and a window that stops overflowing (the phone turned, the dialog closing a pane) is put back.
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {fitOffset, fitScale} from './fitmath.js';

// the window types a user sees as windows: not the launcher (desktop), bars, menus or tooltips
const TYPES = new Set([Meta.WindowType.NORMAL, Meta.WindowType.DIALOG, Meta.WindowType.MODAL_DIALOG,
    Meta.WindowType.UTILITY]);

export class FitWindows {
    // @home: home.js's Home, which knows the launcher's window
    constructor(home) {
        this._home = home;
        this._fitted = new Set();       // the window actors this has scaled
        this._laters = new Map();       // window -> the pending refit
        global.display.connectObject(
            'window-created', (_d, win) => this._watch(win),
            'workareas-changed', () => this._all(), this);
        Main.layoutManager.connectObject('monitors-changed', () => this._all(), this);
        for (const actor of global.get_window_actors())
            this._watch(actor.meta_window);
    }

    _watch(win) {
        if (win.is_override_redirect() || !TYPES.has(win.get_window_type()))
            return;
        win.connectObject(
            'shown', () => this._queue(win),
            'size-changed', () => this._queue(win),
            'notify::minimized', () => this._queue(win),
            'unmanaged', () => this._forget(win), this);
        this._queue(win);
    }

    _all() {
        for (const actor of global.get_window_actors()) {
            if (TYPES.has(actor.meta_window.get_window_type()))
                this._queue(actor.meta_window);
        }
    }

    // not from inside mutter's own change of the size: once it is done
    _queue(win) {
        if (this._laters.has(win))
            return;
        this._laters.set(win, global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._laters.delete(win);
            this._fit(win);
            return GLib.SOURCE_REMOVE;
        }));
    }

    _forget(win) {
        const later = this._laters.get(win);
        if (later)
            global.compositor.get_laters().remove(later);
        this._laters.delete(win);
        const actor = win.get_compositor_private();
        if (actor)
            this._fitted.delete(actor);
        win.disconnectObject(this);
    }

    _fit(win) {
        const actor = win.get_compositor_private();
        if (!actor || win.minimized || this._home.isHome(win))
            return;
        // an animation ending (the map, a launch) puts the scale back to 1: look again then
        if (!actor._fitWatched) {
            actor._fitWatched = true;
            actor.connectObject('notify::scale-x', () => {
                if (actor.scale_x === 1)
                    this._queue(win);
            }, this);
        }
        const area = win.get_work_area_current_monitor();
        const frame = win.get_frame_rect();
        const scale = fitScale(frame.width, frame.height, area.width, area.height);
        if (scale < 1) {
            const offset = fitOffset(scale, frame, win.get_buffer_rect(), area);
            actor.set_pivot_point(0, 0);
            actor.set_scale(scale, scale);
            actor.set_translation(offset.x, offset.y, 0);
            this._fitted.add(actor);
        } else if (this._fitted.delete(actor)) {
            actor.set_scale(1, 1);
            actor.set_translation(0, 0, 0);
        }
    }

    destroy() {
        global.display.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        for (const [win, later] of this._laters)
            global.compositor.get_laters().remove(later);
        this._laters.clear();
        for (const actor of global.get_window_actors()) {
            actor.meta_window.disconnectObject(this);
            actor.disconnectObject(this);
            actor._fitWatched = false;
            if (this._fitted.has(actor)) {
                actor.set_scale(1, 1);
                actor.set_translation(0, 0, 0);
            }
        }
        this._fitted.clear();
    }
}
