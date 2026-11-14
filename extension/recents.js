// Recents: Launcher3's RecentsView in its overview state (spec gestures-recents-back.md §2, §5.8), drawn by the shell
// over the launcher, which stays in BACKGROUND_APP behind it (its wallpaper blurred at depth 0.7).
//
// The swipe up from an app (navigation.js) builds it as soon as the app starts to move: the running app is card 0,
// the older tasks lie to its left at a pitch of 732 px (LTR locales lay recents out right to left), hidden until the
// motion pause "attaches" them. Let go after the pause and the card settles into place, the scrim comes up and the
// overview takes the input: scroll along the cards (spline fling, page snap, stretch at the ends), swipe a card up
// to close its app, tap one to open it, tap the space around them, or swipe up from the bar, to go home.
//
// From the home screen there is no running app: every card waits off screen to the left, the scrim follows the
// launcher's hint, and on the pause the cards come in together (enterFromHome).
//
// Lengths are device px (1080 × 2280); actors are placed in stage units (÷ the monitor scale).
import Clutter from 'gi://Clutter';
import GdkPixbuf from 'gi://GdkPixbuf';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {TICK, vibrate} from './haptics.js';
import {ICONS} from './icondata.js';
import {Canvas, VectorIcon} from './icons.js';
import {ICONS as L3_ICONS} from './l3icondata.js';
import {PathInterpolator} from './launch.js';
import {VelocityTracker} from './motion.js';
import {Color, cogl, gild, setSource} from './palette.js';
import {overWindows} from './scanout.js';
import {openAppInfo, settingsApp} from './settings.js';
import {
    ACCELERATE_2, ACCELERATE_DECELERATE, DECELERATE, DECELERATE_3, FAST_OUT_SLOW_IN, LINEAR, NAV_GESTURE_H,
    OVERSHOOT_1_2, SCREEN_H, SCREEN_W, SCROLL, Spring, TASK, TASK_CORNER_R, TASK_PIVOT, TASK_W, TOUCH_RESPONSE,
    TaskView, animate, appGicon, clamp, clampToProgress, lerp, scaleRect, springCurve,
} from './task.js';
import {AndroidText} from './text.js';
import {USER} from './stacks-model.js';
import {screenPixels} from './units.js';

Object.assign(ICONS, L3_ICONS);

// TEXTURE_TICK at twice its amplitude (see _scrollOver)
const SCROLL_TICK = TICK.map(([amplitude, ms]) => [amplitude * 2, ms]);
const PITCH = 732;                      // the card, 684, and the page spacing, 16 dp
const TOUCH_SLOP = 24;                  // 8 dp
const CARD_H = TASK[3] - TASK[1];
const CARD_CENTER_Y = (TASK[1] + TASK[3]) / 2;

// the scrim over the launcher's blurred wallpaper: the dark surface, a dark blur (the recording's (67, 91, 95)
// read as light gray here)
const SCRIM = Color.surface;
const SCRIM_ALPHA = 0.77;
// HintState's: the overview's colour at alpha round(0.77 × 100) / 255, as a fraction of the overview's
export const HINT_SCRIM = Math.round(SCRIM_ALPHA * 100) / 255 / SCRIM_ALPHA;

// entering (§1.3, §5.8; the clip ran these longer than the code: settle 435 vs ≤ 350, attach 390 vs 300, chip
// fade 185 vs 120)
export const SETTLE_MS = 435;
const ATTACH = springCurve([250, 0.8], 1 / 500);
const ATTACH_MS = 390;
const DETACH_MS = 300;
const CHIP_FADE_MS = 185;

// the app chip (IconAppChipView, collapsed): a pill over the card's top-left, inset 36 px (4 dp + its background's
// 8 dp), with the icon, the app's name, then (the owner's) its memory use, and the menu's chevron 8 dp from the end.
// Launcher3's is 156 dp wide whatever the name; this one is no narrower and grows to fit the name and the memory, up
// to the card's width less the insets.
const CHIP = {inset: 36, minW: 464, h: 108, icon: 72, iconX: 18, textX: 114, memGap: 24, arrow: 72, arrowEnd: 24};
const CHIP_COLOUR = [34, 44, 45];       // materialColorSurfaceBright, measured
const CHIP_RADIUS = 84;                 // app_chip_round_corner_radius 28 dp: a pill, collapsed or expanded
// expanded (revealAnim, for the menu): the background 8 dp out at the top and start, 52 dp tall and at least 216 dp
// wide; the icon 32 dp; the name's start moves by the icon's growth over two, the chevron to the new end, upside down
const CHIP_OPEN = {out: 24, h: 156, minW: 648, icon: 96};
// the menu (TaskMenuView, enableOverviewIconMenu): 216 dp wide, 4 dp from the card's start and 2 dp under the
// expanded chip; it opens from 6 dp higher (menuToCollapsedChipGap) and 8 dp further in, its outline from the
// collapsed chip's width and 0.8 of its height, the rows inside from 0.8 scale. One row an option, 52 dp: the icon
// 20 dp at 16 dp, the label 14 sp 18 dp after it (all as measured on the S10e)
const MENU = {x: 12, y: 174, w: 648, fromX: 36, fromY: 156, fromW: 468, from: 0.8, r: 84, rowH: 156, icon: 60,
    iconX: 48, textX: 162};
const MENU_OPEN_MS = 417, MENU_CLOSE_MS = 333;
const MENU_DIM = 0.4;                   // the card under it: TaskThumbnailView's MAX_SCRIM_ALPHA
const PRESSED_ALPHA = 0.15;             // app_chip_menu_item_color_fg: onSurface
const EMPHASIZED = new PathInterpolator('M0,0 C0.05,0 0.133333,0.06 0.166666,0.4 C0.208333,0.82 0.25,1 1,1');
// the panel's alpha and the rows', on EMPHASIZED's progress
const MENU_ALPHA = clampToProgress(LINEAR, 0.48, 0.74), ROWS_ALPHA = clampToProgress(LINEAR, 0.75, 1);
// the actions row (OverviewActionsView): the Screenshot pill; Clear all is the same button style. Launcher3 has it
// at y 1812, 72 px under the card; here the memory map (RamMap) sits between them, MAP_MARGIN from each, and the row
// goes down by the map's height (Recents._actions)
const ACTIONS = [329, 1812, 750, 1956];
const MAP_MARGIN = 42;                  // 14 dp
const BUTTON_COLOUR = [31, 41, 42];
const BUTTON_H = 144, BUTTON_PAD = 48, BUTTON_ICON = 60, BUTTON_ICON_GAP = 24;
const DISABLED_ALPHA = 0.29;
const CLEAR_ALL_LEFT = 150;             // its left edge when scrolled to it: the oldest card's left − 48 − its width
// no tasks: the carousel icon (120 dp) over "No recent items", the screen's middle (less half the status bar) between
// them
const EMPTY_ICON = 360, EMPTY_PAD = 48, STATUS_BAR_H = 116;

// scrolling (PagedView in free scroll; OverScroller's spline; OverScroll.dampedScroll; EdgeEffect's recede)
const SNAP_MIN_MS = 270;
// … and the pager the swipe up drives (paging mode: quick switch), PagedView's: in screen px and px/s
const PAGER_SLOP = 24, PAGER_PAGE_SLOP = 48;                            // touch slop, paging touch slop
const PAGER_FLING = 1500, PAGER_MIN_FLING = 750, PAGER_MIN_SNAP_V = 4500;
const PAGER_SIGNIFICANT = 0.4, PAGER_RETURN = 0.33;
export const PAGER_SNAP_MS = 750;
const FRICTION = 0.015, PHYS = 155670.6, DECEL = Math.log(0.78) / Math.log(0.9), INFLEXION = 0.35;
const OVERSCROLL = 0.07 * SCREEN_W;
const EDGE = [24.657 * 24.657, 0.98];

// swipe-up dismiss (TaskViewDismissTouchController, RecentsDismissUtils; reflow measured 600/0.8 against the code's
// 2800)
const DIS_DETACH_PX = 216, DIS_PRE_DETACH = 0.35, DIS_DETACH = [800, 0.95];
const DIS_UNDERSHOOT = 75, DIS_LENGTH = 1785, DIS_THRESHOLD = 892, DIS_FLING = 3;
const DIS_Y = [850, 0.65], DIS_V_MIN = 1444, DIS_REFLOW = [600, 0.8], DIS_RSCALE = [200, 0.75];
const DIS_NEIGHBOUR_DZETA = 0.15;

// leaving
const LAUNCH_MS = 478;                  // TOUCH_RESPONSE; the code says 336, the clip 478
const EXIT_MS = 250;                    // OVERVIEW → NORMAL
const EXIT_SCROLL_PAGE_MS = 150, EXIT_SCROLL_MAX_MS = 750;
const HOME_SWIPE_SCALE = 0.25;          // FROM_OVERVIEW: the recents scale 1 → 0.75 over the screen's height
const HOME_SWIPE_TY = -211.9;           // … and up by half the way to the top
const HOME_SWIPE_FLING = 3;             // px/ms
const PULLBACK_MS = 80;
// from home, once there: the cards back from under the finger at 0.8 dp/ms, in no less than 80 ms
const DRAG_SETTLE_MIN_MS = 80, DRAG_SETTLE_PX_MS = 2.4;

// ---- OverScroller's spline ----------------------------------------------------------------------------------------

const SPLINE = (() => {
    const n = 100, p1 = 0.5 * INFLEXION, p2 = 1 - (1 - INFLEXION);
    const table = new Float64Array(n + 1);
    let xMin = 0;
    for (let i = 0; i < n; i++) {
        const alpha = i / n;
        let xMax = 1, x, coef;
        for (;;) {
            x = xMin + (xMax - xMin) / 2;
            coef = 3 * x * (1 - x);
            const tx = coef * ((1 - x) * p1 + x * p2) + x * x * x;
            if (Math.abs(tx - alpha) < 1e-5)
                break;
            if (tx > alpha)
                xMax = x;
            else
                xMin = x;
        }
        table[i] = coef * ((1 - x) * 0.5 + x) + x * x * x;
    }
    table[n] = 1;
    return table;
})();

function splinePosition(t) {
    const n = SPLINE.length - 1, i = Math.floor(n * t);
    if (i >= n)
        return 1;
    return SPLINE[i] + (t * n - i) * (SPLINE[i + 1] - SPLINE[i]);
}

// the distance (px) and time (ms) a fling at v px/s coasts
function fling(v) {
    const l = Math.log(INFLEXION * Math.abs(v) / (FRICTION * PHYS));
    return {
        distance: FRICTION * PHYS * Math.exp(DECEL / (DECEL - 1) * l),
        duration: 1000 * Math.exp(l / (DECEL - 1)),
    };
}

const dampedScroll = pull => Math.sign(pull) * OVERSCROLL * (1 - Math.pow(1 - Math.min(Math.abs(pull) / SCREEN_W, 1), 3));

// the recents view's scale while a card is dragged up, by how far it is out of the way
function dismissScale(f) {
    if (f <= 0.2)
        return lerp(1, 0.9875, f / 0.2);
    if (f <= 0.5)
        return 0.9875;
    if (f <= 0.575)
        return lerp(0.9875, 0.975, (f - 0.5) / 0.075);
    return 0.975;
}

// ---- chrome -------------------------------------------------------------------------------------------------------

// The labels' baseline sits this far (em) below the middle of what they are centred in: the cap height's half, as
// measured on the chip and the Screenshot button
const BASELINE_BELOW_CENTRE = 0.357;

// The app chip over a card's top-left: the icon, the name, the memory and the chevron on a pill (see CHIP). Its
// origin is the collapsed pill's top-left; expand() opens it out for the menu (IconAppChipView.revealAnim).
class Chip {
    constructor(app, path, scale, parent) {
        const u = v => v / scale;
        this._scale = scale;
        this._open = 0;
        this.root = new Clutter.Actor({
            opacity: 0, offscreen_redirect: Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY,
        });
        // St keeps the radius to half the height: a pill at either height
        this._pill = new St.Widget({
            style: `background-color: rgb(${CHIP_COLOUR}); border-radius: ${u(CHIP_RADIUS)}px;${gild()}`,
        });
        this.root.add_child(this._pill);
        // app_chip_fg: pressed, onSurface at 0.15 over it
        this._press = new St.Widget({
            style: `background-color: rgba(${Color.onSurface}, ${PRESSED_ALPHA}); border-radius: ${u(CHIP_RADIUS)}px;`,
            opacity: 0,
        });
        this.root.add_child(this._press);
        const gicon = appGicon(app, path);
        if (gicon) {
            this._icon = new St.Icon({gicon, icon_size: Math.round(u(CHIP.icon)), style: 'padding: 0;'});
            this._icon.set_pivot_point(0.5, 0.5);
            this._icon.set_position(u(CHIP.iconX), u((CHIP.h - CHIP.icon) / 2));
            this.root.add_child(this._icon);
        }
        // IconAppChipMenuTextStyle: google-sans-text-medium 14 sp, letterSpacing 0.025, made bold by the phone
        this._name = new AndroidText(app?.get_name() ?? '', 14, 500, {colour: Color.onSurface, embolden: true,
            letterSpacing: 0.025});
        this._nameClip = new Clutter.Actor({height: u(CHIP.h), clip_to_allocation: true});
        this._name.set_position(u(-this._name.inset), u(this._labelTop(this._name)));
        this._nameClip.add_child(this._name);
        this.root.add_child(this._nameClip);
        // the memory: the same 14 sp, not bold, in the secondary text colour; empty until it has been read
        this._memory = new AndroidText('', 14, 500, {colour: Color.onSurfaceVariant});
        this.root.add_child(this._memory);
        this._arrow = new VectorIcon('ic_chevron_down', u(CHIP.arrow), Color.onSurface);
        this._arrow.set_pivot_point(0.5, 0.5);
        this.root.add_child(this._arrow);
        this._maxW = TASK_W - 2 * CHIP.inset;
        this._fit();
        parent.add_child(this.root);
    }

    setMemory(bytes) {
        this._memory.setText(formatBytes(bytes));
        this._fit();
    }

    // a label's top, screen px, for its baseline to sit where the chip's name does
    _labelTop(text) {
        return CHIP.h / 2 + BASELINE_BELOW_CENTRE * 42 - text.baseline - text.inset;
    }

    // the collapsed pill's width for the name and the memory; the name gives way
    _fit() {
        const mem = this._memory.textWidth ? CHIP.memGap + this._memory.textWidth : 0;
        const fixed = CHIP.textX + mem + CHIP.arrow + CHIP.arrowEnd;
        this._nameW = Math.max(0, Math.min(this._name.textWidth, this._maxW - fixed));
        this.width = Math.max(CHIP.minW, fixed + this._nameW);
        this._openW = Math.max(CHIP_OPEN.minW, this.width + 2 * CHIP_OPEN.out);
        this._layout();
    }

    _layout() {
        const u = v => v / this._scale;
        const e = this._open;
        const out = CHIP_OPEN.out * e, h = lerp(CHIP.h, CHIP_OPEN.h, e);
        const w = lerp(this.width, this._openW, e);
        this._pill.set_position(u(-out), u(-out));
        this._pill.set_size(u(w), u(h));
        this._press.set_position(u(-out), u(-out));
        this._press.set_size(u(w), u(h));
        if (this._icon)
            this._icon.scale_x = this._icon.scale_y = lerp(1, CHIP_OPEN.icon / CHIP.icon, e);
        const dx = (CHIP_OPEN.icon - CHIP.icon) / 2 * e;
        this._nameClip.set_position(u(CHIP.textX + dx), 0);
        this._nameClip.width = u(this._nameW);
        this._memory.set_position(u(CHIP.textX + dx + this._nameW + CHIP.memGap - this._memory.inset),
            u(this._labelTop(this._memory)));
        // the chevron keeps its distance from the background's end, and turns over (scaleY 1 -> -1)
        this._arrow.set_position(u(w - out - CHIP.arrowEnd - CHIP.arrow), u((CHIP.h - CHIP.arrow) / 2));
        this._arrow.scale_y = 1 - 2 * e;
    }

    // e: 0 collapsed … 1 open
    expand(e) {
        if (e === this._open)
            return;
        this._open = e;
        this._layout();
    }

    pressed(on) {
        this._press.opacity = on ? 255 : 0;
    }

    // at the card's top-left, scaled with it; rect: where it takes a touch (the view's: from the card's corner
    // 4 dp in, 52 dp tall)
    place(rect, s, alpha) {
        this.root.visible = alpha > 0;
        if (!alpha) {
            this.rect = null;
            return;
        }
        const sc = this._scale;
        const x = rect[0] + CHIP.inset * s, y = rect[1] + CHIP.inset * s;
        this.root.set_pivot_point(0, 0);
        this.root.scale_x = this.root.scale_y = s;
        this.root.set_position(x / sc, y / sc);
        this.root.opacity = Math.round(255 * alpha);
        const out = CHIP_OPEN.out * s;
        this.rect = [x - out, y - out, x + this.width * s, y + (CHIP_OPEN.h - CHIP_OPEN.out) * s];
    }

    destroy() {
        this.root.destroy();
    }
}

// The chip's menu (TaskMenuView): a panel of the chip's colour, one row an option (see MENU). items: {icon, label,
// run}
class TaskMenu {
    constructor(items, scale, parent) {
        const u = v => v / scale;
        this._scale = scale;
        this.items = items;
        this.height = items.length * MENU.rowH;
        this.root = new St.Widget({
            style: `background-color: rgb(${CHIP_COLOUR}); border-radius: ${u(MENU.r)}px;${gild()}`,
            clip_to_allocation: true, opacity: 0,
            offscreen_redirect: Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY,
        });
        this._rows = new Clutter.Actor({width: u(MENU.w), height: u(this.height)});
        this._rows.set_pivot_point(0, 0);
        this.root.add_child(this._rows);
        this._pressed = items.map((item, i) => {
            const top = i * MENU.rowH;
            // app_chip_menu_item_bg pressed: a pill of onSurface at 0.15, .5 dp in at the sides
            const press = new St.Widget({
                style: `background-color: rgba(${Color.onSurface}, ${PRESSED_ALPHA}); ` +
                    `border-radius: ${u(CHIP_RADIUS)}px;`,
                x: u(1.5), y: u(top), width: u(MENU.w - 3), height: u(MENU.rowH), opacity: 0,
            });
            this._rows.add_child(press);
            const icon = new VectorIcon(item.icon, u(MENU.icon), Color.onSurface);
            icon.set_position(u(MENU.iconX), u(top + (MENU.rowH - MENU.icon) / 2));
            this._rows.add_child(icon);
            // the label: 14 sp in the default sans (BaseIcon), bold on the phone: 600 has its widths, made bold; its
            // baseline where the chip's would be in a row this tall
            const text = new AndroidText(item.label, 14, 600, {colour: Color.onSurface, embolden: true});
            text.set_position(u(MENU.textX - text.inset),
                u(top + MENU.rowH / 2 + BASELINE_BELOW_CENTRE * 42 - text.baseline - text.inset));
            this._rows.add_child(text);
            return press;
        });
        parent.add_child(this.root);
    }

    // card: the card's rect; s its scale; p 0 … 1 the reveal; alpha the panel's, rowsAlpha the rows'
    place(card, s, p, alpha, rowsAlpha) {
        const u = v => v / this._scale;
        const x = card[0] + lerp(MENU.fromX, MENU.x, p) * s, y = card[1] + lerp(MENU.fromY, MENU.y, p) * s;
        const w = lerp(MENU.fromW, MENU.w, p) * s, h = lerp(MENU.from, 1, p) * this.height * s;
        this.root.set_position(u(x), u(y));
        this.root.set_size(u(w), u(h));
        this.root.opacity = Math.round(255 * alpha);
        this._rows.scale_x = this._rows.scale_y = lerp(MENU.from, 1, p) * s;
        this._rows.opacity = Math.round(255 * rowsAlpha);
        this.rect = [x, y, x + w, y + h];
    }

    // the option under (x, y), screen px, or -1
    itemAt(x, y) {
        const r = this.rect;
        if (!r || x < r[0] || x >= r[2] || y < r[1] || y >= r[3])
            return -1;
        return clamp(Math.floor((y - r[1]) / ((r[3] - r[1]) / this.items.length)), 0, this.items.length - 1);
    }

    press(i) {
        this._pressed.forEach((p, k) => (p.opacity = k === i ? 255 : 0));
    }

    destroy() {
        this.root.destroy();
    }
}

// ---- memory -----------------------------------------------------------------------------------------------------
//
// Each app's memory beside its name, and the phone's memory as a coloured map above the Screenshot button: the GJS
// launcher's recents (its RamBar), same design.

const RAM_COLOURS = {used: [0xef, 0x53, 0x50], cache: [0xff, 0xb7, 0x4d], free: [0x66, 0xbb, 0x6a]};
const RAM = {barH: 30, gap: 18, legendGap: 42, lineGap: 6, badgePadX: 24, badgePadY: 3, badgeBorder: 3};
const APPMEM_TIMEOUT_S = 3;

// the GJS launcher's formatBytes: binary units, whole megabytes, gigabytes to one place
function formatBytes(n) {
    return n >= 1024 ** 3 ? `${(n / 1024 ** 3).toFixed(1)} GB` : `${Math.round(n / 1024 ** 2)} MB`;
}

// /proc/meminfo in bytes, by key: the kernel's own file, read in place (it never blocks)
function meminfo() {
    const out = {};
    const [, data] = GLib.file_get_contents('/proc/meminfo');
    for (const line of new TextDecoder().decode(data).split('\n')) {
        const m = line.match(/^(\w+):\s+(\d+)/);
        if (m)
            out[m[1]] = Number(m[2]) * 1024;
    }
    return out;
}

// what appmem.py measures a window by: its process, or an Android app's package (Waydroid's window belongs to its
// bridge process, the same for every app)
function memoryKey(win, app) {
    const id = app?.get_id() ?? '';
    return id.startsWith('waydroid.') ? id.replace(/\.desktop$/, '') : `${win.get_pid()}`;
}

// Runs appmem.py for @keys in a process of its own, under a timeout (see its header); @done gets {key: bytes};
// @failed, when given, is called instead when there is no answer (the helper did not start, or died)
export function measureApps(keys, done, failed = () => {}) {
    const helper = GLib.build_filenamev([GLib.path_get_dirname(import.meta.url.replace('file://', '')),
        'appmem.py']);
    let proc;
    try {
        proc = Gio.Subprocess.new(['timeout', '-s', 'KILL', `${APPMEM_TIMEOUT_S}`, 'python3', helper, ...keys],
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
    } catch (e) {
        logError(e, 'gnome-phone: appmem');
        failed();
        return;
    }
    proc.communicate_utf8_async(null, null, (pr, res) => {
        try {
            done(JSON.parse(pr.communicate_utf8_finish(res)[1] || '{}'));
        } catch {
            // killed by the timeout, or nothing printed: no figures this time
            failed();
        }
    });
}

// The phone's memory: a bar of used (apps, the kernel, GPU buffers: what cannot be dropped), cache (file pages the
// kernel can drop at once) and free, and under it a legend, centred: the whole memory as a badge on the left (no
// colour of its own, it is not a part of the bar), the three parts on the right, one per line. Shared memory counts
// as used, not cache: here that is mostly GPU buffers, which the kernel cannot reclaim.
class RamMap {
    constructor(scale, parent) {
        const u = v => v / scale;
        this._scale = scale;
        this.root = new Clutter.Actor({opacity: 0, offscreen_redirect: Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY});

        this._bar = new Canvas({width: u(TASK_W), height: u(RAM.barH)});
        this._bar.draw = cr => this._paintBar(cr);
        this.root.add_child(this._bar);

        // the legend's texts: 12 sp, the badge bold and white, the parts at 85 % white with their colour's dot
        this._badge = new St.Widget({
            style: `border: ${u(RAM.badgeBorder)}px solid rgba(255, 255, 255, 0.6); border-radius: 999px;`,
        });
        this._total = new AndroidText('', 12, 700, {colour: Color.white});
        this._badge.add_child(this._total);
        this.root.add_child(this._badge);
        this._parts = ['used', 'cache', 'free'].map(key => {
            const dot = new AndroidText('●', 12, 400, {colour: RAM_COLOURS[key]});
            const label = new AndroidText('', 12, 400, {colour: Color.white, alpha: 0.85});
            this.root.add_child(dot);
            this.root.add_child(label);
            return {key, dot, label};
        });
        this._m = null;
        parent.add_child(this.root);
        this.refresh();
    }

    // the map's height, screen px: the bar, the gap, the legend's three lines
    get height() {
        const line = this._parts[0].label.textHeight;
        return RAM.barH + RAM.gap + 3 * line + 2 * RAM.lineGap;
    }

    refresh() {
        let mi;
        try {
            mi = meminfo();
        } catch (e) {
            logError(e, 'gnome-phone: meminfo');
            return;
        }
        const total = mi.MemTotal || 0;
        if (!total)
            return;
        const free = mi.MemFree || 0;
        const cache = Math.max(0, (mi.Buffers || 0) + (mi.Cached || 0) + (mi.SReclaimable || 0) - (mi.Shmem || 0));
        const used = Math.max(0, total - free - cache);
        this._m = {total, used, cache, free};
        this._total.setText(formatBytes(total));
        const names = {used: 'Used', cache: 'Cache', free: 'Free'};
        for (const part of this._parts)
            part.label.setText(` ${names[part.key]} ${formatBytes(this._m[part.key])}`);
        this._layout();
        this._bar.queue_repaint();
    }

    // the legend centred under the bar: the badge, 14 dp, the parts' column; the badge centred on the column
    _layout() {
        const u = v => v / this._scale;
        const line = this._parts[0].label.textHeight;
        const top = RAM.barH + RAM.gap;
        const columnH = 3 * line + 2 * RAM.lineGap;
        const badgeW = this._total.textWidth + 2 * (RAM.badgePadX + RAM.badgeBorder);
        const badgeH = this._total.textHeight + 2 * (RAM.badgePadY + RAM.badgeBorder);
        const dotW = this._parts[0].dot.textWidth;
        const columnW = dotW + Math.max(...this._parts.map(p => p.label.textWidth));
        const x0 = (TASK_W - (badgeW + RAM.legendGap + columnW)) / 2;

        this._badge.set_position(u(x0), u(top + (columnH - badgeH) / 2));
        this._badge.set_size(u(badgeW), u(badgeH));
        this._total.set_position(u(RAM.badgePadX + RAM.badgeBorder - this._total.inset),
            u(RAM.badgePadY + RAM.badgeBorder - this._total.inset));
        const x1 = x0 + badgeW + RAM.legendGap;
        this._parts.forEach(({dot, label}, i) => {
            const y = top + i * (line + RAM.lineGap);
            dot.set_position(u(x1 - dot.inset), u(y - dot.inset));
            label.set_position(u(x1 + dotW - label.inset), u(y - label.inset));
        });
    }

    // the three parts in one rounded track
    _paintBar(cr) {
        if (!this._m)
            return;
        screenPixels(cr);
        const {total} = this._m, h = RAM.barH, r = h / 2;
        cr.newPath();
        cr.arc(r, r, r, 0.5 * Math.PI, 1.5 * Math.PI);
        cr.arc(TASK_W - r, r, r, 1.5 * Math.PI, 0.5 * Math.PI);
        cr.closePath();
        cr.clip();
        let x = 0;
        for (const key of ['used', 'cache', 'free']) {
            const w = TASK_W * this._m[key] / total;
            setSource(cr, RAM_COLOURS[key]);
            cr.rectangle(x, 0, w + 0.5, h);
            cr.fill();
            x += w;
        }
    }

    // top: the bar's top edge, screen px; the map is a card wide, at the resting card's x
    place(top, alpha) {
        this.root.visible = alpha > 0;
        if (!alpha)
            return;
        this.root.set_position(TASK[0] / this._scale, top / this._scale);
        this.root.opacity = Math.round(255 * alpha);
    }

    destroy() {
        this.root.destroy();
    }
}

// An overview action button: a pill of the actions row's colour, an optional icon, and its label
class Button {
    constructor(text, iconName, scale, parent) {
        const u = v => v / scale;
        this._scale = scale;
        this.root = new St.Widget({
            height: u(BUTTON_H), opacity: 0,
            style: `background-color: rgb(${BUTTON_COLOUR}); border-radius: ${u(BUTTON_H / 2)}px;${gild(0.5)}`,
            offscreen_redirect: Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY,
        });
        // OverviewActionButton: 14 sp at 600, which the phone draws as 700 made bold; the icon, then 8 dp, then
        // the label, centred in the pill
        this._content = new Clutter.Actor();
        this._text = new AndroidText(text, 14, 700, {colour: Color.onSurface, embolden: true});
        let x = 0;
        if (iconName) {
            this._icon = new VectorIcon(iconName, u(BUTTON_ICON), Color.onSurface);
            this._icon.set_position(0, u((BUTTON_H - BUTTON_ICON) / 2));
            this._content.add_child(this._icon);
            x = BUTTON_ICON + BUTTON_ICON_GAP;
        }
        this._text.set_position(u(x - this._text.inset),
            u(BUTTON_H / 2 + BASELINE_BELOW_CENTRE * 42 - this._text.baseline - this._text.inset));
        this._content.add_child(this._text);
        this.root.add_child(this._content);
        parent.add_child(this.root);
        this.contentWidth = x + this._text.textWidth;
        this.width = this.contentWidth + 2 * BUTTON_PAD;
    }

    // rect: the pill (device px)
    place(rect, alpha, enabled = true) {
        this.rect = rect;
        this.root.visible = alpha > 0;
        if (!alpha)
            return;
        const sc = this._scale;
        this.root.set_position(rect[0] / sc, rect[1] / sc);
        this.root.set_size((rect[2] - rect[0]) / sc, (rect[3] - rect[1]) / sc);
        this._content.set_position(((rect[2] - rect[0]) - this.contentWidth) / 2 / sc,
            ((rect[3] - rect[1]) - BUTTON_H) / 2 / sc);
        this.root.opacity = Math.round(255 * alpha);
        this._content.opacity = Math.round(255 * (enabled ? 1 : DISABLED_ALPHA));
    }

    destroy() {
        this.root.destroy();
    }
}

let taskIds = 0;

// Loads the fonts the chips and the buttons are set in, so the first swipe up does not wait ~25 ms for them
// ---- the stacks' tabs ---------------------------------------------------------------------------------------------

const TABS = {top: STATUS_BAR_H + 24, h: 96, inset: 36, gap: 18, padX: 42};

// One pill per owner in a row above the cards; the selected one in the accent. A tap on a pill is recents' to take
// (itemAt).
class StackTabs {
    constructor(scale, parent) {
        this._scale = scale;
        this.alpha = 0;
        this.root = new Clutter.Actor({opacity: 0});
        parent.add_child(this.root);
        this._items = [];
    }

    // items: [{owner, count}], selected: an owner
    set(items, selected) {
        const u = v => v / this._scale;
        for (const old of this.root.get_children())
            old.destroy();
        let x = TABS.inset;
        this._items = items.map(({owner, count}) => {
            const on = owner === selected;
            const name = owner === USER ? 'You' : owner.charAt(0).toUpperCase() + owner.slice(1);
            const label = new AndroidText(`${name}  ${count}`, 14, 500, {
                colour: on ? Color.onPrimary : Color.onSurface, embolden: true, letterSpacing: 0.025});
            const w = Math.round(label.textWidth + 2 * TABS.padX);
            const pill = new St.Widget({
                style: `background-color: rgb(${on ? Color.primary : CHIP_COLOUR}); border-radius: ${u(TABS.h / 2)}px;`,
            });
            pill.set_position(u(x), 0);
            pill.set_size(u(w), u(TABS.h));
            label.set_position(u(x + TABS.padX - label.inset), u((TABS.h - label.textHeight) / 2 - label.inset));
            this.root.add_child(pill);
            this.root.add_child(label);
            const item = {owner, rect: [x, TABS.top, x + w, TABS.top + TABS.h]};
            x += w + TABS.gap;
            return item;
        });
        this.root.set_position(0, u(TABS.top));
    }

    place(alpha) {
        this.alpha = alpha;
        this.root.visible = alpha > 0;
        this.root.opacity = Math.round(255 * alpha);
    }

    itemAt(x, y) {
        return this._items.find(i => x >= i.rect[0] && x < i.rect[2] && y >= i.rect[1] && y < i.rect[3])?.owner ?? null;
    }

    destroy() {
        this.root.destroy();
    }
}

export function warmUp() {
    new AndroidText('Recents', 14, 500, {colour: Color.onSurface, embolden: true, letterSpacing: 0.025}).destroy();
    new AndroidText('Clear all', 14, 700, {colour: Color.onSurface, embolden: true}).destroy();
}

// ---- the overview -------------------------------------------------------------------------------------------------

// A swiped-away app gets ActivityManager's treatment: asked to close, and its process killed when it hasn't after
// KILL_MS (a game ignoring the request, an app stuck). Its window, if it goes, takes the kill with it. A SIGKILL, not
// mutter's kill: for a Wayland client that is a protocol error, which a stuck client never reads.
const KILL_MS = 5000;
const closing = new Map();      // window -> the kill's timeout

export function closeApp(win) {
    if (closing.has(win))
        return;
    const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, KILL_MS, () => {
        closing.delete(win);
        win.disconnectObject(closing);
        const pid = win.get_pid();
        if (pid > 1 && pid !== new Gio.Credentials().get_unix_pid()) {
            log(`gnome-phone: ${win.get_wm_class()} (pid ${pid}) did not close, killing it`);
            Gio.Subprocess.new(['kill', '-KILL', `${pid}`], Gio.SubprocessFlags.NONE);
        }
        return GLib.SOURCE_REMOVE;
    });
    closing.set(win, id);
    win.connectObject('unmanaged', () => {
        GLib.source_remove(closing.get(win));
        closing.delete(win);
        win.disconnectObject(closing);
    }, closing);
    win.delete(global.get_current_time());
}

// the extension going away: no app killed after it
export function forgetClosing() {
    for (const [win, id] of closing) {
        GLib.source_remove(id);
        win.disconnectObject(closing);
    }
    closing.clear();
}

export class Recents {
    // nav: the gesture navigation (the launcher, its icons, the home); running: the app the swipe started over, or
    // null from the home screen
    constructor(nav, running) {
        this._nav = nav;
        this._scale = nav.scale;
        const m = Main.layoutManager.primaryMonitor;
        this.layer = new Clutter.Actor({name: 'gnomePhoneRecents', reactive: true,
            x: m.x, y: m.y, width: m.width, height: m.height});
        Main.layoutManager.uiGroup.insert_child_above(this.layer, global.window_group);
        overWindows(this.layer);
        this._scrim = new Clutter.Actor({width: m.width, height: m.height, background_color: cogl(SCRIM), opacity: 0});
        this.layer.add_child(this._scrim);

        // the cards of one stack (stacks.js): the running app's, when there are owners besides the user
        this._stacks = nav.stacks ?? null;
        this._stacks?.setOverview(true);        // the agent chip steps aside while recents is up
        this._running = running;
        this._stack = running && this._stacks ? this._stacks.ownerOf(running) : USER;
        this._tasks = [];
        this._buildTasks();
        this._syncEmpty();
        this._clearAll = new Button('Clear all', null, this._scale, this.layer);
        this._screenshot = new Button('Screenshot', 'ic_screenshot', this._scale, this.layer);

        // the phone's memory between the cards and the Screenshot pill, which moves down to make room
        this._ram = new RamMap(this._scale, this.layer);
        this._ramTop = TASK[3] + MAP_MARGIN;
        const down = this._ramTop + this._ram.height + MAP_MARGIN - ACTIONS[1];
        this._actions = [ACTIONS[0], ACTIONS[1] + down, ACTIONS[2], ACTIONS[3] + down];

        this._measureMemory();

        // the stacks as tabs over the cards: only when an agent has a window
        this._tabs = null;
        this._syncTabs();
        // where the running card is (TaskViewSimulator); from home, where card 0 rests
        this._frame = running ? [0, 0, SCREEN_W, SCREEN_H] : TASK.slice();
        this._frameR = running ? 0 : TASK_CORNER_R;
        this._scroll = 0;                           // px along the cards, 0 = the running one centred
        this._pull = 0;                             // px dragged past an end
        this._offset = 1;                           // ADJACENT_PAGE_HORIZONTAL_OFFSET: 1 = the others off screen
        this._content = 0;                          // the other cards' alpha
        this._stripScale = 1;                       // RECENTS_SCALE about _stripPivot
        this._stripPivot = TASK_PIVOT;
        this._stripTy = 0;
        this._dragTx = 0;                           // from home: where the finger has the cards since the pause
        this._dragTy = 0;
        this._exitTx = 0;
        this._chipAlpha = 0;
        this._actionsAlpha = 0;
        this._scrolling = false;
        this._overPage = 0;                         // the page nearest the middle, for its tick
        this._overTick = 0;                         // µs, monotonic
        this._pager = null;                         // the swipe's horizontal travel, screen px
        this._restWaiters = [];
        this._runningTaken = false;
        this._motions = new Map();
        this._ticker = null;
        this._tracker = new VelocityTracker();
        this._touch = null;
        this._busy = false;
        this.shown = false;

        global.display.connectObject('window-created', (_d, win) => {
            if (win.get_window_type() === Meta.WindowType.NORMAL)
                this._interrupted();
        }, this);
        this.layout();
    }

    // The selected stack's cards: the running task first (when it is this stack's), then the others, most recent
    // first (from every workspace: the mobile shell gives each app its own)
    _buildTasks() {
        const nav = this._nav, running = this._running;
        const inStack = win => !this._stacks || this._stacks.ownerOf(win) === this._stack;
        const first = running && inStack(running) ? running : null;
        const tracker = Shell.WindowTracker.get_default();
        const others = global.display.get_tab_list(Meta.TabList.NORMAL, null)
            .filter(w => w !== running && nav.isTask(w) && inStack(w));
        this._fromHome = !first;
        this._tasks = (first ? [first, ...others] : others).map(win => {
            const app = tracker.get_window_app(win);
            const path = app ? nav.icons.get(app.get_id())?.path : null;
            const view = new TaskView(win, this._scale, this.layer);
            const chip = new Chip(app, path || null, this._scale, this.layer);
            const task = {id: ++taskIds, win, app, view, chip, dx: 0, dy: 0, alpha: 1, radius: null};
            win.connectObject('unmanaged', () => this._lost(task), this);
            return task;
        });
        this._runningTask = first ? this._tasks[0] : null;
    }

    _syncEmpty() {
        this._empty?.destroy();
        this._empty = this._tasks.length ? null : this._emptyMessage();
    }

    // each app's memory beside its name, measured once per opening (and again for another stack)
    _measureMemory() {
        const keys = this._tasks.map(task => (task.memoryKey = memoryKey(task.win, task.app)));
        if (!keys.length)
            return;
        measureApps([...new Set(keys)], memory => {
            if (this._destroyed)
                return;
            for (const task of this._tasks) {
                if (memory[task.memoryKey])
                    task.chip.setMemory(memory[task.memoryKey]);
            }
        });
    }

    _syncTabs() {
        const owners = this._stacks?.owners() ?? [USER];
        if (owners.length < 2) {
            this._tabs?.destroy();
            this._tabs = null;
            return;
        }
        this._tabs ??= new StackTabs(this._scale, this.layer);
        this._tabs.set(owners.map(owner => ({owner, count: this._stacks.count(owner)})), this._stack);
    }

    // A tab: the cards of another stack. The app the swipe started over is not in it, so it goes to recents like
    // the others, with the launcher behind.
    _switchStack(owner) {
        if (owner === this._stack || this._busy || this._menu || this._reflowing)
            return;
        // every app that was on screen or under it goes to recents, as at home: their cards are about to go, and a
        // window that was only hidden by its card would show through the scrim
        const wins = new Set(this._tasks.filter(t => !t.dismissed).map(t => t.win));
        if (this._running)
            wins.add(this._running);
        let moved = false;
        for (const win of wins) {
            const actor = win.get_compositor_private();
            if (win.minimized || !actor)
                continue;
            Main.wm.skipNextEffect(actor);
            win.minimize();
            moved = true;
        }
        if (moved)
            this._nav.home.window?.activate(global.get_current_time());
        this._running = null;
        for (const name of [...this._motions.keys()]) {
            if (name === 'scroll' || name === 'edge' || name.startsWith('dy'))
                this._stop(name);
        }
        for (const task of this._tasks) {
            task.win.disconnectObject(this);
            task.view.destroy();
            task.chip.destroy();
        }
        this._stack = owner;
        this._scroll = 0;
        this._pull = 0;
        this._scrolling = false;
        this._buildTasks();
        this._syncEmpty();
        this._measureMemory();
        this._syncTabs();
        this._overPage = this._nearestPage();
        this.layout();
    }

    // RecentsView's empty message: label-large 14 sp at 400, made 700 by the phone
    _emptyMessage() {
        const u = v => v / this._scale;
        const root = new Clutter.Actor({opacity: 0});
        const icon = new VectorIcon('ic_view_carousel', u(EMPTY_ICON), Color.onSurface);
        const text = new AndroidText('No recent items', 14, 700, {colour: Color.onSurface});
        const top = (SCREEN_H - (text.textHeight + EMPTY_PAD + EMPTY_ICON)) / 2 + STATUS_BAR_H / 2 -
            (EMPTY_ICON + EMPTY_PAD) / 2;
        icon.set_position(u((SCREEN_W - EMPTY_ICON) / 2), u(top));
        text.set_position(u((SCREEN_W - text.textWidth) / 2 - text.inset), u(top + EMPTY_ICON + EMPTY_PAD - text.inset));
        root.add_child(icon);
        root.add_child(text);
        this.layer.add_child(root);
        return root;
    }

    get running() {
        return this._tasks[0]?.view ?? null;
    }

    // ---- the frame clock: every motion steps, then one layout -----------------------------------------------------

    _run(name, step, done = null) {
        this._motions.set(name, {step, done});
        this._ticker ??= animate(this.layer, 'recents', dt => this._tick(dt), () => {});
    }

    _stop(name) {
        this._motions.delete(name);
    }

    _tick(dt) {
        if (this._destroyed)
            return false;
        for (const [name, motion] of [...this._motions]) {
            if (this._motions.get(name) !== motion)
                continue;
            if (motion.step(dt) === false) {
                this._motions.delete(name);
                motion.done?.();
                if (this._destroyed)
                    return false;
            }
        }
        this.layout();
        this._checkRest();
        if (this._destroyed)
            return false;
        if (this._motions.size)
            return true;
        this._ticker = null;
        return false;
    }

    // fn(t) over ms (t 0 … 1 linear in time)
    _tween(name, ms, fn, done) {
        let elapsed = 0;
        fn(0);
        this._run(name, dt => {
            elapsed += dt * 1000;
            const t = ms > 0 ? clamp(elapsed / ms, 0, 1) : 1;
            fn(t);
            return t < 1;
        }, done);
    }

    _spring(name, spring, fn, done) {
        this._run(name, dt => {
            spring.advance(dt);
            fn(spring.x, spring.v);
            return !spring.done;
        }, done);
    }

    // ---- where everything is ------------------------------------------------------------------------------------

    _over() {
        return dampedScroll(this._pull);
    }

    _maxScroll() {
        return PITCH * (this._tasks.length - 1) + this._clearAll.width;
    }

    _clearAllAlpha() {
        if (!this._tasks.length)
            return 0;
        return clamp(1 - (this._maxScroll() - this._scroll) / 48, 0, 1);
    }

    _page() {
        return clamp(Math.round(this._scroll / PITCH), 0, this._tasks.length - 1);
    }

    _pageScrolls() {
        return [...this._tasks.map((_t, i) => PITCH * i), this._maxScroll()];
    }

    // the page (a card, or Clear all) nearest the middle of the screen
    _nearestPage() {
        const pages = this._pageScrolls();
        let page = 0;
        pages.forEach((p, k) => {
            if (Math.abs(p - this._scroll) < Math.abs(pages[page] - this._scroll))
                page = k;
        });
        return page;
    }

    // PagedView.onScrollChanged -> RecentsView.onScrollOverPageChanged: as another page comes nearest the middle (a
    // card crosses halfway) under the finger or a fling, vibrateForScroll: PRIMITIVE_LOW_TICK, or on a vibrator
    // without primitives (the S10e's) EFFECT_TEXTURE_TICK, no sooner than 20 ms after the last
    // (recentsScrollHapticMinGapMillis). At twice Android's amplitude: the owner found it too subtle (2026-11-09).
    _scrollOver() {
        const page = this._nearestPage();
        if (page === this._overPage)
            return;
        this._overPage = page;
        const now = GLib.get_monotonic_time();
        if (now - this._overTick < 20000)
            return;
        this._overTick = now;
        vibrate(SCROLL_TICK);
    }

    layout() {
        if (this._destroyed)
            return;
        const [l, t, , b] = this._frame;
        const s = (this._frame[2] - l) / TASK_W;
        const sc = this._stripScale, pivot = this._stripPivot;
        const shift = this._scroll + this._over();
        const left = i => l + (-PITCH * i + shift) * s;
        // the cards left of the running one go off screen together, by as much as takes the nearest one there (from
        // home, all of them)
        const first = this._fromHome ? 0 : 1;
        const push = this._offset > 0 && this._tasks.length > first
            ? -(left(first) + TASK_W * s) * this._offset : 0;
        const place = rect => {
            const r = scaleRect(rect, sc, pivot);
            r[0] += this._dragTx;
            r[2] += this._dragTx;
            r[1] += this._stripTy + this._dragTy;
            r[3] += this._stripTy + this._dragTy;
            return r;
        };
        this._tasks.forEach((task, i) => {
            const x = left(i) + task.dx * s + (i >= first ? push : 0) + this._exitTx;
            const y = t + task.dy * s;
            const rect = place([x, y, x + TASK_W * s, y + (b - t)]);
            const alpha = (i >= first ? this._content : 1) * task.alpha;
            const r = (task.radius ?? this._frameR) * sc;
            if (!(i === 0 && this._runningTaken))
                task.view.place(rect, r, alpha);
            task.chip.place(rect, s * sc, this._chipAlpha * alpha);
        });

        // Clear all, past the oldest card: it stays put while the last card slides off it, fading in over the
        // last 48 px
        const max = this._maxScroll(), w = this._clearAll.width;
        const ca = this._clearAllAlpha();
        const cx = CLEAR_ALL_LEFT + (this._scroll - max) + Math.min(Math.max(max - this._scroll, 0), w) + this._over();
        this._clearAll.place(place([cx + this._exitTx, CARD_CENTER_Y - BUTTON_H / 2, cx + w + this._exitTx,
            CARD_CENTER_Y + BUTTON_H / 2]), ca * this._actionsAlpha * this._content);
        // the actions hide with no tasks
        this._screenshot.place(this._actions, this._tasks.length ? this._actionsAlpha * (1 - ca) : 0, !this._scrolling);
        this._ram.place(this._ramTop, this._actionsAlpha);
        this._tabs?.place(this._actionsAlpha * this._content);
        if (this._empty)
            this._empty.opacity = Math.round(255 * this._content);
        if (this._menu)
            this._placeMenu();
    }

    // ---- driven by the swipe up ---------------------------------------------------------------------------------

    // the running card is at rect with corners r (on screen); the others follow it
    follow(rect, r) {
        this._frame = rect;
        this._frameR = r;
        this.layout();
    }

    // the motion pause: the others spring in from off screen, opaque at once
    attach() {
        if (this._attached)
            return;
        this._attached = true;
        this._content = 1;
        const from = this._offset;
        this._tween('attach', ATTACH_MS, t => (this._offset = from * ATTACH(t)));
    }

    // the gesture went home or back to the app: the others spring back out and fade
    detach() {
        if (!this._attached)
            return;
        this._attached = false;
        const from = this._offset, alpha = this._content;
        this._tween('attach', DETACH_MS, t => {
            this._offset = 1 - (1 - from) * ATTACH(t);
            this._content = alpha * (1 - ACCELERATE_2(t));
        });
    }

    // From home, on the pause (HINT → OVERVIEW): the cards slide in from the left on OVERSHOOT_1_2, opaque at once
    // (there are cards to scroll in), the scrim comes up to the overview's on the same curve, never past it, and the
    // actions fade in LINEAR
    enterFromHome(ms, done) {
        this._content = 1;
        this._chipAlpha = 1;
        const scrim = this._scrim.opacity / 255 / SCRIM_ALPHA;
        this._tween('enter', ms, t => {
            const o = OVERSHOOT_1_2(t);
            this._offset = 1 - o;
            this.setScrim(lerp(scrim, 1, Math.min(o, 1)));
            this._actionsAlpha = t;
        }, done);
    }

    // … there, the finger still down: tx, ty px, or rise (a fraction of the way up) for the overview's resistance
    drag(tx, ty, rise) {
        this._dragTx = tx;
        this._dragTy = ty;
        this._homeSwipe(rise);
        this.layout();
    }

    // … and let go: back into place on ACCELERATE_DECELERATE
    settleDrag(done) {
        const tx = this._dragTx, ty = this._dragTy, rise = this._homeT ?? 0;
        const ms = Math.max(DRAG_SETTLE_MIN_MS, Math.max(Math.abs(tx), Math.abs(ty)) / DRAG_SETTLE_PX_MS);
        this._tween('drag', ms, t => {
            const e = 1 - ACCELERATE_DECELERATE(t);
            this._dragTx = tx * e;
            this._dragTy = ty * e;
            this._homeSwipe(rise * e);
        }, done);
    }

    // the scrim, as a fraction of the overview's
    setScrim(f) {
        this._scrim.opacity = Math.round(255 * SCRIM_ALPHA * f);
    }

    // the running card is the window's now, flying home: recents no longer places it
    takeRunning() {
        this._runningTaken = true;
        return this.running;
    }

    // The settle is over: the overview takes the input. Its actions show at once, the chips fade in.
    show() {
        this.shown = true;
        this._frame = TASK.slice();
        this._frameR = TASK_CORNER_R;
        this._actionsAlpha = 1;
        if (this._chipAlpha < 1)
            this._tween('chips', CHIP_FADE_MS, t => (this._chipAlpha = LINEAR(t)));
        this._grab = Main.pushModal(this.layer, {actionMode: Shell.ActionMode.NONE});
        this.layer.grab_key_focus();
        // the grab keeps touches from the stage: back watches them here
        this._nav.watchBack(this.layer);
        // this Clutter emits only the generic signal
        this.layer.connectObject('event', (_a, event) => {
            switch (event.type()) {
            case Clutter.EventType.TOUCH_BEGIN:
            case Clutter.EventType.TOUCH_UPDATE:
            case Clutter.EventType.TOUCH_END:
            case Clutter.EventType.TOUCH_CANCEL:
                return this._onTouch(event);
            case Clutter.EventType.KEY_PRESS:
                return this._onKey(event);
            default:
                return Clutter.EVENT_PROPAGATE;
            }
        }, this);
    }

    // ---- input --------------------------------------------------------------------------------------------------

    _hit(x, y) {
        if (y >= SCREEN_H - NAV_GESTURE_H)
            return {kind: 'bar'};
        const inside = r => r && x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
        if (this._clearAllAlpha() >= 1 && inside(this._clearAll.rect))
            return {kind: 'clear-all'};
        if (this._screenshot.root.visible && this._clearAllAlpha() < 1 && inside(this._actions))
            return {kind: 'screenshot'};
        const tab = this._tabs && this._tabs.alpha >= 1 ? this._tabs.itemAt(x, y) : null;
        if (tab)
            return {kind: 'tab', owner: tab};
        // the chip opens the menu once the chips are in (a card's own touch otherwise)
        const chip = this._chipAlpha >= 1 && this._tasks.find(t => !t.dismissed && inside(t.chip.rect));
        if (chip)
            return {kind: 'chip', task: chip};
        const task = this._tasks.find(t => t.view.root.visible && inside(t.view.rect));
        return task ? {kind: 'card', task} : {kind: 'empty'};
    }

    _onTouch(event) {
        if (this._busy || (this._reflowing && !this._touch))
            return Clutter.EVENT_STOP;
        // GJS wraps a sequence anew for every event: its slot is what stays the same
        const type = event.type(), seq = event.get_event_sequence()?.get_slot() ?? 0;
        const [sx, sy] = event.get_coords();
        const x = sx * this._scale, y = sy * this._scale, time = event.get_time();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            if (this._touch)
                return Clutter.EVENT_STOP;
            // the menu open (AbstractFloatingView): a touch on an option is its tap; one anywhere else closes it
            // and is spent, but for a swipe from the navigation bar
            if (this._menu) {
                const item = this._menu.closing ? -1 : this._menu.menu.itemAt(x, y);
                if (item >= 0 || this._hit(x, y).kind !== 'bar') {
                    this._touch = {seq, x0: x, y0: y, x, y, mode: 'menu', item, hit: {kind: 'menu'}};
                    if (item >= 0)
                        this._menu.menu.press(item);
                    else
                        this._closeMenu();
                    return Clutter.EVENT_STOP;
                }
                this._closeMenu();
            }
            // a touch stops a fling; that touch scrolls, it does not tap
            const caught = this._motions.has('scroll');
            this._stop('scroll');
            this._stop('edge');
            this._touch = {seq, x0: x, y0: y, x, y, mode: caught ? 'scroll' : null, hit: this._hit(x, y)};
            if (this._touch.hit.kind === 'chip')
                this._touch.hit.task.chip.pressed(true);
            this._tracker.clear();
            this._tracker.add(time, x, y);
            if (caught) {
                this._scrolling = true;
                return Clutter.EVENT_STOP;
            }
            // not claimed until it scrolls, dismisses or goes home: an actor handling an event drops the gestures
            // tracking it (back, from the edges)
            return Clutter.EVENT_PROPAGATE;
        }
        const touch = this._touch;
        if (!touch || seq !== touch.seq)
            return Clutter.EVENT_STOP;
        if (type === Clutter.EventType.TOUCH_UPDATE) {
            if (touch.mode === 'menu') {
                if (touch.item >= 0 && Math.hypot(x - touch.x0, y - touch.y0) >= TOUCH_SLOP) {
                    touch.item = -1;
                    this._menu?.menu.press(-1);
                }
                return Clutter.EVENT_STOP;
            }
            this._tracker.add(time, x, y);
            const dx = x - touch.x, dy = y - touch.y;
            touch.x = x;
            touch.y = y;
            if (!touch.mode) {
                const tx = x - touch.x0, ty = y - touch.y0;
                if (Math.hypot(tx, ty) < TOUCH_SLOP)
                    return Clutter.EVENT_PROPAGATE;
                if (touch.hit.kind === 'chip')
                    touch.hit.task.chip.pressed(false);
                if (touch.hit.kind === 'bar')
                    touch.mode = ty < 0 ? 'home' : 'none';
                else if (touch.hit.kind === 'tab')
                    touch.mode = 'none';
                else if (Math.abs(tx) > Math.abs(ty))
                    touch.mode = 'scroll';
                else if (touch.hit.kind === 'card' || touch.hit.kind === 'chip')
                    touch.mode = 'dismiss';
                else
                    touch.mode = 'none';
                if (touch.mode === 'scroll')
                    this._scrolling = true;
                else if (touch.mode === 'dismiss')
                    this._dismissStart(touch.hit.task);
            }
            if (touch.mode === 'scroll')
                this._scrollBy(dx);
            else if (touch.mode === 'dismiss')
                this._dismissMove(y - touch.y0);
            else if (touch.mode === 'home')
                this._homeSwipe(clamp((touch.y0 - y) / SCREEN_H, 0, 1));
            this.layout();
            return Clutter.EVENT_STOP;
        }
        if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.TOUCH_CANCEL) {
            this._touch = null;
            const cancelled = type === Clutter.EventType.TOUCH_CANCEL;
            if (touch.hit.kind === 'chip')
                touch.hit.task.chip.pressed(false);
            if (touch.mode === 'menu') {
                const menu = this._menu;
                menu?.menu.press(-1);
                if (menu && !menu.closing && touch.item >= 0 && !cancelled)
                    menu.menu.items[touch.item].run();
                return Clutter.EVENT_STOP;
            }
            const vx = this._tracker.velocityX, vy = this._tracker.velocityY;
            switch (touch.mode) {
            case 'scroll':
                this._release(cancelled ? 0 : vx);
                break;
            case 'dismiss':
                this._dismissRelease(cancelled ? 0 : vy / 1000);
                break;
            case 'home':
                this._homeSwipeRelease(cancelled ? 0 : vy / 1000);
                break;
            case null:
                if (!cancelled)
                    this._tap(touch.hit);
                break;
            }
        }
        return Clutter.EVENT_STOP;
    }

    _onKey(event) {
        if (this._busy)
            return Clutter.EVENT_STOP;
        const key = event.get_key_symbol();
        // Escape, and the Bixby key again (keys.js AppSwitchKey), its repeats while held dropped
        if (key === Clutter.KEY_Escape ||
            (key === Clutter.KEY_AppSelect && !(event.get_flags() & Clutter.EventFlags.FLAG_REPEATED)))
            this.back();
        return Clutter.EVENT_STOP;
    }

    // back (RecentsState.onBackInvoked): to the app recents was opened over when its card is wholly on screen, to
    // its card when it is not, home when it is gone
    back() {
        if (this._busy)
            return;
        if (this._menu) {
            if (!this._menu.closing)
                this._closeMenu();
            return;
        }
        const task = this._runningTask;
        if (!this._tasks.includes(task) || task.dismissed)
            this.goHome();
        else if (task.view.rect && task.view.rect[0] >= 0 && task.view.rect[2] <= SCREEN_W)
            this._launch(task);
        else
            this.snapTo(this._tasks.indexOf(task), PAGER_SNAP_MS);
    }

    _tap(hit) {
        switch (hit.kind) {
        case 'chip':
            this._openMenu(hit.task);
            break;
        case 'card':
            this._launch(hit.task);
            break;
        case 'clear-all':
            this._clearAllTasks();
            break;
        case 'screenshot':
            this._takeScreenshot();
            break;
        case 'tab':
            this._switchStack(hit.owner);
            break;
        case 'empty':
            this.goHome();
            break;
        }
    }

    // ---- scrolling ----------------------------------------------------------------------------------------------

    _scrollBy(dx) {
        if (this._pull) {
            // let go of the stretch first
            const p = this._pull + dx;
            if (Math.sign(p) === Math.sign(this._pull)) {
                this._pull = p;
                return;
            }
            this._pull = 0;
            dx = p;
        }
        const max = this._maxScroll();
        let s = this._scroll + dx;
        if (s < 0) {
            this._pull = s;
            s = 0;
        } else if (s > max) {
            this._pull = s - max;
            s = max;
        }
        this._scroll = s;
        this._scrollOver();
    }

    // the finger left at v px/s: the stretch recedes; the scroll coasts on the spline to the nearest page, in no
    // less than 270 ms
    _release(v) {
        if (this._pull) {
            const edge = new Spring(EDGE, this._pull, 0, 0, 1);
            this._spring('edge', edge, x => (this._pull = x), () => (this._pull = 0));
        }
        const f = fling(v);
        const aim = this._scroll + Math.sign(v) * (Number.isFinite(f.distance) ? f.distance : 0);
        const pages = this._pageScrolls();
        const end = pages.reduce((a, p) => Math.abs(p - aim) < Math.abs(a - aim) ? p : a, pages[0]);
        this._scrollTo(end, Math.max(SNAP_MIN_MS, Number.isFinite(f.duration) ? f.duration : 0), splinePosition);
    }

    _scrollTo(end, ms, curve, done = null) {
        const start = this._scroll;
        if (Math.abs(end - start) < 0.5) {
            this._scroll = end;
            this._scrolling = false;
            done?.();
            return;
        }
        this._scrolling = true;
        this._tween('scroll', ms, t => {
            this._scroll = lerp(start, end, curve(t));
            this._scrollOver();
        }, () => {
            this._scrolling = false;
            done?.();
        });
    }

    // ---- the pager, while the swipe up is still going (quick switch) ----------------------------------------------

    // The finger moved dx px along the bar, the cards at scale s. Past the touch slop RecentsView scrolls with it,
    // 1:1 on screen.
    pagerMove(dx, s) {
        const pager = this._pager ??= {travel: 0, motion: 0, dragging: false, s};
        pager.s = s;
        pager.travel += dx;
        pager.motion += Math.abs(dx);
        if (!pager.dragging) {
            if (Math.abs(pager.travel) <= PAGER_SLOP)
                return;
            pager.dragging = true;
        }
        this._stop('edge');
        this._scrollBy(dx / s);
    }

    // whether the finger has scrolled the strip
    get pagerDragged() {
        return !!this._pager?.dragging;
    }

    // the page nearest the middle of the screen
    centredPage() {
        return this._page();
    }

    // The finger left at vx px/s: PagedView's ACTION_UP in paging mode. A fling, or a drag past 40 % of a card, goes
    // one page on (finger right: older, recents running right to left), but back if the fling went against a drag
    // of a third of a card; the duration follows the fling (snapToPageWithVelocity). Otherwise the nearest page, in
    // 750 ms. The stretch past an end recedes. Returns the page.
    pagerRelease(vx) {
        const pager = this._pager, n = this._tasks.length;
        this._pager = null;
        if (this._pull) {
            const edge = new Spring(EDGE, this._pull, 0, 0, 1);
            this._spring('edge', edge, x => (this._pull = x), () => (this._pull = 0));
        }
        if (!pager?.dragging) {
            this.snapTo(0, PAGER_SNAP_MS);
            return 0;
        }
        const card = TASK_W * pager.s, travel = pager.travel;
        const fling = pager.motion > PAGER_PAGE_SLOP && Math.abs(vx) > PAGER_FLING;
        const significant = Math.abs(travel) > PAGER_SIGNIFICANT * card;
        const back = fling && Math.abs(travel) > PAGER_RETURN * card && Math.sign(vx) !== Math.sign(travel);
        if (((significant && travel > 0 && !fling) || (fling && vx > 0)) && n > 1) {
            const page = back ? 0 : 1;
            if (Math.abs(vx) < PAGER_MIN_FLING) {
                this.snapTo(page, PAGER_SNAP_MS);
            } else {
                const ratio = Math.min(1, Math.abs(page * PITCH - this._scroll) * pager.s / SCREEN_W);
                const distance = SCREEN_W / 2 * (1 + Math.sin((ratio - 0.5) * 0.3 * Math.PI / 2));
                this.snapTo(page, 4 * Math.round(1000 * distance / Math.max(PAGER_MIN_SNAP_V, Math.abs(vx))));
            }
            return page;
        }
        const page = this._page();
        this.snapTo(page, PAGER_SNAP_MS);
        return page;
    }

    // PagedView's snapToPage: SCROLL
    snapTo(page, ms) {
        this._scrollTo(page * PITCH, ms, SCROLL);
    }

    // done() once the strip is at rest: no scroll, no stretch
    afterScroll(done) {
        this._restWaiters.push(done);
        this._checkRest();
    }

    _checkRest() {
        if (!this._restWaiters.length || this._motions.has('scroll') || this._motions.has('edge'))
            return;
        const waiters = this._restWaiters;
        this._restWaiters = [];
        for (const done of waiters)
            done();
    }

    task(page) {
        return this._tasks[page] ?? null;
    }

    // ---- swipe a card up to close its app ---------------------------------------------------------------------

    _dismissStart(task) {
        this._stop(`dy${task.id}`);
        this._dis = {task, detached: false, spring: null, target: 0};
    }

    // fy: the finger's travel along y since it went down (negative up)
    _dismissMove(fy) {
        const dis = this._dis, task = dis.task;
        if (fy > 0) {
            // down: a little give, decelerating
            task.dy = DECELERATE(Math.min(fy, DIS_LENGTH) / DIS_LENGTH) * DIS_UNDERSHOOT;
        } else if (!dis.detached && -fy < DIS_DETACH_PX) {
            task.dy = DIS_PRE_DETACH * fy;
        } else {
            dis.target = fy;
            if (!dis.detached) {
                // past 72 dp it lets go of the finger's 0.35 and springs to 1:1, for good
                dis.detached = true;
                dis.spring = new Spring(DIS_DETACH, task.dy, 0, fy);
                this._run('detach', dt => {
                    dis.spring.animateTo(dis.target);
                    task.dy = dis.spring.advance(dt);
                    this._stripScale = dismissScale(clamp(-task.dy / DIS_LENGTH, 0, 1));
                    return this._dis === dis;
                });
            }
        }
        this._stripScale = dismissScale(clamp(-task.dy / DIS_LENGTH, 0, 1));
    }

    // vy: px/ms
    _dismissRelease(vy) {
        const dis = this._dis;
        this._dis = null;
        this._stop('detach');
        const task = dis.task;
        const dismiss = vy < -DIS_FLING || (-task.dy > DIS_THRESHOLD && !(vy > DIS_FLING));
        const rscale = new Spring(DIS_RSCALE, this._stripScale, 0, 1, 1 / 1000);
        this._spring('rscale', rscale, x => (this._stripScale = x));
        if (dismiss) {
            this._dismiss(task, -Math.max(Math.abs(vy * 1000), DIS_V_MIN));
            return;
        }
        // back into place; crossing it kicks the neighbours
        const back = new Spring(DIS_Y, task.dy, Math.sign(-task.dy) * Math.max(Math.abs(vy * 1000), DIS_V_MIN), 0);
        let kicked = false;
        this._spring(`dy${task.id}`, back, (x, v) => {
            if (!kicked && Math.sign(x) !== Math.sign(task.dy) && task.dy !== 0) {
                kicked = true;
                this._kick(task, v);
            }
            task.dy = x;
        });
    }

    // the neighbours' translationY follows a driver kicked with v, each a little more damped than the last
    _kick(kicked, v) {
        const i = this._tasks.indexOf(kicked);
        const driver = new Spring(DIS_Y, 0, v, 0);
        const chains = [-1, 1].map(dir => {
            const springs = [];
            for (let k = 1; k <= 3; k++) {
                const task = this._tasks[i + dir * k];
                if (task)
                    springs.push({task, spring: new Spring([DIS_Y[0], DIS_Y[1] + DIS_NEIGHBOUR_DZETA * k], 0, 0, 0)});
            }
            return springs;
        });
        this._run(`kick${kicked.id}`, dt => {
            driver.advance(dt);
            let busy = !driver.done;
            for (const chain of chains) {
                let lead = driver.x;
                for (const {task, spring} of chain) {
                    spring.animateTo(lead);
                    task.dy = spring.advance(dt);
                    lead = spring.x;
                    busy ||= !spring.done;
                }
            }
            return busy;
        });
    }

    // The card flies off the top on the dismiss spring, its app closes, and the cards beside it close the gap.
    // v0: px/s (negative)
    _dismiss(task, v0) {
        const fly = new Spring(DIS_Y, task.dy, v0, -DIS_LENGTH);
        let flowing = false;
        task.dismissed = true;
        this._reflowing = true;
        this._spring(`dy${task.id}`, fly, (x, v) => {
            task.dy = x;
            // past one card height
            if (!flowing && x <= -CARD_H) {
                flowing = true;
                this._reflow(task, Math.abs(v));
            }
            if (x <= -DIS_LENGTH)
                fly.done = true;
        }, () => {
            task.alpha = 0;
            this._close(task);
            if (!flowing)
                this._reflow(task, 0);
        });
    }

    // a card swiped or cleared away: its app goes whether or not its fly-off ran to the end (a lone card's reflow
    // has nothing to move and removes it at once, ending the fly-off before its close)
    _close(task) {
        if (task.closed)
            return;
        task.closed = true;
        closeApp(task.win);
        // the app's memory comes back once it has gone: read the map again a moment later
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
            if (!this._destroyed)
                this._ram.refresh();
            return GLib.SOURCE_REMOVE;
        });
    }

    // RecentsDismissUtils: the older cards to the left slide right into the hole (or, when it was the oldest or
    // left of the current page, the newer ones slide left), the first on a spring, each next one chasing it
    _reflow(task, v0) {
        const i = this._tasks.indexOf(task), n = this._tasks.length, page = this._page();
        const newer = i === n - 1 || i > page;
        const dir = newer ? -1 : 1;
        const movers = [];
        for (let j = newer ? i - 1 : i + 1; j >= 0 && j < n; j += newer ? -1 : 1)
            movers.push({task: this._tasks[j], spring: null});
        movers.forEach((m, k) => {
            m.spring = new Spring(DIS_REFLOW, 0, k === 0 ? dir * v0 : 0, dir * PITCH);
        });
        const finish = () => this._removeTask(task, newer);
        if (!movers.length) {
            finish();
            return;
        }
        this._run(`reflow${task.id}`, dt => {
            let lead = null, busy = false;
            for (const m of movers) {
                if (lead !== null)
                    m.spring.animateTo(lead);
                m.task.dx = m.spring.advance(dt);
                lead = m.spring.x;
                busy ||= !m.spring.done;
            }
            return busy;
        }, finish);
    }

    _removeTask(task, newerMoved) {
        const i = this._tasks.indexOf(task);
        if (i < 0)
            return;
        // the fly-off is done by now; end it if not
        this._stop(`dy${task.id}`);
        if (task.dismissed && !task.lost)
            this._close(task);
        this._reflowing = false;
        this._tasks.splice(i, 1);
        task.win.disconnectObject(this);
        task.view.destroy();
        task.chip.destroy();
        for (const t of this._tasks) {
            t.dx = 0;
            if (!t.dismissed)
                t.dy = 0;
        }
        if (newerMoved)
            this._scroll = Math.max(0, this._scroll - PITCH);
        this._scroll = Math.min(this._scroll, this._maxScroll());
        // the pages moved, not the strip: no tick
        this._overPage = this._nearestPage();
        if (!this._tasks.length && !this._busy)
            this.goHome();
        else
            this.layout();
    }

    // the window closed by itself
    _lost(task) {
        task.lost = true;
        if (this._menu?.task === task)
            this._dropMenu();
        if (task.dismissed)
            return;
        task.dismissed = true;
        this._removeTask(task, false);
    }

    _clearAllTasks() {
        this._busy = true;
        let left = 0;
        this._tasks.forEach(task => {
            if (!task.view.root.visible) {
                this._close(task);
                return;
            }
            left++;
            const fly = new Spring(DIS_Y, task.dy, -DIS_V_MIN, -DIS_LENGTH);
            task.dismissed = true;
            this._spring(`dy${task.id}`, fly, x => {
                task.dy = x;
                if (x <= -DIS_LENGTH)
                    fly.done = true;
            }, () => {
                task.alpha = 0;
                this._close(task);
                if (--left === 0)
                    this.goHome('home', true);
            });
        });
        if (!left)
            this.goHome('home', true);
    }

    // ---- leaving --------------------------------------------------------------------------------------------------

    // Open a card's app: the whole strip scales by 1080/684 about the point that takes this card onto the screen,
    // the card's corners square off, the chips and actions fade at once
    _launch(task) {
        // switching to a task is a launch on Android too (Mode::LAUNCH)
        this._nav.boost?.launch();
        this._busy = true;
        this._stop('scroll');
        this._stop('chips');
        const [x0, y0, x1] = task.view.rect;
        const full = SCREEN_W / (x1 - x0);
        const pivot = [x0 * full / (full - 1), y0 * full / (full - 1)];
        this._stop('rscale');
        this._stripPivot = pivot;
        this._stripScale = 1;
        const win = task.win;
        if (win.minimized)
            Main.wm.skipNextEffect(win.get_compositor_private());
        win.activate(global.get_current_time());
        const chips = this._chipAlpha, actions = this._actionsAlpha;
        const fade = clampToProgress(LINEAR, 0, 0.15);
        this._tween('launch', LAUNCH_MS, t => {
            const p = TOUCH_RESPONSE(t);
            this._stripScale = lerp(1, full, p);
            task.radius = lerp(TASK_CORNER_R, 0, p) / this._stripScale;
            this._chipAlpha = chips * (1 - fade(t));
            this._actionsAlpha = actions * (1 - fade(t));
        }, () => {
            this._nav.launcherState('normal');
            this._nav.statusBarOverApp(null);
            this.destroy();
        });
    }

    // OVERVIEW → NORMAL: the cards slide off to the left, the scrim and the actions fade, the launcher's workspace
    // grows back in behind (kind 'home': 0.92 → 1 in 250 ms; 'home-reveal', after a swipe up: WorkspaceRevealAnim)
    goHome(kind = 'home', closing = false) {
        this._busy = true;
        this._stop('scroll');
        this._stop('enter');
        this._stop('drag');
        this._nav.launcherState(kind, GLib.get_monotonic_time());
        for (const task of this._tasks) {
            const actor = task.win.get_compositor_private();
            if (!task.win.minimized && !closing && actor) {
                Main.wm.skipNextEffect(actor);
                task.win.minimize();
            }
        }
        this._nav.home.window?.activate(global.get_current_time());
        this._nav.statusBarOverApp(null);

        const pages = this._page();
        const ms = Math.max(EXIT_MS, Math.min(EXIT_SCROLL_MAX_MS, pages * EXIT_SCROLL_PAGE_MS));
        const scroll = this._scroll;
        const current = this._tasks[pages]?.view.rect;
        const away = -(current ? current[2] : TASK[2]);
        const scrim = this._scrim.opacity, actions = this._actionsAlpha;
        const slide = clampToProgress(FAST_OUT_SLOW_IN, 0, 0.75), fade = clampToProgress(LINEAR, 0, EXIT_MS / ms * 0.25);
        this._tween('exit', ms, t => {
            this._exitTx = away * slide(t);
            this._scroll = lerp(scroll, 0, SCROLL(t));
            this._scrim.opacity = Math.round(scrim * (1 - t));
            this._actionsAlpha = actions * (1 - fade(t));
        }, () => this.destroy());
    }

    // Swiping up from the bar: the cards shrink toward 0.75 and rise (FROM_OVERVIEW resistance); t = |dy|/H
    _homeSwipe(t) {
        this._homeT = t;
        this._stripPivot = TASK_PIVOT;
        this._stripScale = 1 - HOME_SWIPE_SCALE * DECELERATE(t);
        this._stripTy = HOME_SWIPE_TY * t;
    }

    _homeSwipeRelease(vy) {
        const t = this._homeT ?? 0;
        if (DECELERATE_3(t) >= 0.5 || vy < -HOME_SWIPE_FLING) {
            this.goHome(vy < 0 ? 'home-reveal' : 'home');
            return;
        }
        this._tween('pullback', PULLBACK_MS, u => this._homeSwipe(t * (1 - u)));
    }

    // The window was asked for elsewhere (a notification, a launch): get out of its way at once
    _interrupted() {
        if (!this.shown || this._busy)
            return;
        this._nav.launcherState('normal');
        this._nav.statusBarOverApp(null);
        this.destroy();
    }

    // ---- the chip's menu -------------------------------------------------------------------------------------

    // TaskOverlayFactory.getEnabledShortcuts, as LineageOS 23.2 lists them, less what this shell cannot do: App
    // info; Split screen (no split screen here); Pin and Freeform (off on Android too until turned on); Screenshot;
    // Clear (TaskViewShortFactory's remove task)
    _menuItems(task) {
        const items = [];
        const id = task.app && !task.app.is_window_backed() ? task.app.get_id() : null;
        if (id) {
            // the app's page in Settings, over the home screen (where back from it leads on Android)
            // (info_24px: SystemShortcut.AppInfo's with the visual refresh, which the phone has)
            items.push({icon: 'info_24px', label: 'App info', run: () => {
                this._closeMenu();
                this.goHome();
                const settings = settingsApp();
                if (settings)
                    this._nav.expectWindow?.(settings);
                openAppInfo(id);
            }});
        }
        items.push({icon: 'ic_screenshot', label: 'Screenshot', run: () => {
            this._closeMenu();
            this._takeScreenshot(task);
        }});
        items.push({icon: 'ic_remove_task_option', label: 'Clear', run: () => {
            this._closeMenu();
            this._dismiss(task, -DIS_V_MIN);
        }});
        return items;
    }

    // TaskMenuView.showForTask with the icon menu: the chip opens out, the menu grows from under it and the card
    // dims, 417 ms on EMPHASIZED (the dim on time). The menu stays where it opened; the chip and the dim go with the
    // card.
    _openMenu(task) {
        if (this._menu || this._busy || task.dismissed || !task.view.rect)
            return;
        const menu = new TaskMenu(this._menuItems(task), this._scale, this.layer);
        const dim = new St.Widget({
            style: `background-color: black; border-radius: ${TASK_CORNER_R / this._scale}px;`, opacity: 0,
        });
        this.layer.insert_child_above(dim, task.view.root);
        // the chip over the menu (bringToFront)
        this.layer.set_child_above_sibling(task.chip.root, menu.root);
        const card = task.view.rect.slice();
        const m = this._menu = {task, menu, dim, card, s: (card[2] - card[0]) / TASK_W, p: 0, alpha: 0, rowsAlpha: 0,
            dimP: 0, closing: false};
        this._tween('menu', MENU_OPEN_MS, t => {
            const q = EMPHASIZED.get(t);
            m.p = q;
            m.alpha = MENU_ALPHA(q);
            m.rowsAlpha = ROWS_ALPHA(q);
            m.dimP = t;
        });
    }

    // closing: 333 ms on EMPHASIZED from wherever the opening got to
    _closeMenu() {
        const m = this._menu;
        if (!m || m.closing)
            return;
        m.closing = true;
        const {p, alpha, rowsAlpha, dimP} = m;
        this._tween('menu', MENU_CLOSE_MS, t => {
            const q = EMPHASIZED.get(t);
            m.p = p * (1 - q);
            m.alpha = alpha * (1 - MENU_ALPHA(q));
            m.rowsAlpha = rowsAlpha * (1 - ROWS_ALPHA(q));
            m.dimP = dimP * (1 - t);
        }, () => this._dropMenu());
    }

    _dropMenu() {
        const m = this._menu;
        if (!m)
            return;
        this._stop('menu');
        this._menu = null;
        m.menu.destroy();
        m.dim.destroy();
        m.task.chip.expand(0);
    }

    _placeMenu() {
        const m = this._menu, task = m.task;
        m.menu.place(m.card, m.s, m.p, m.alpha, m.rowsAlpha);
        task.chip.expand(m.p);
        const rect = task.view.rect;
        m.dim.visible = !!rect && task.view.root.visible && m.dimP > 0;
        if (!m.dim.visible)
            return;
        const sc = this._scale;
        m.dim.set_position(rect[0] / sc, rect[1] / sc);
        m.dim.set_size((rect[2] - rect[0]) / sc, (rect[3] - rect[1]) / sc);
        m.dim.opacity = Math.round(255 * MENU_DIM * m.dimP * task.alpha);
    }

    // ---- Screenshot ---------------------------------------------------------------------------------------------

    // OverviewActionsView's Screenshot: the current card's app, as it is, into ~/Pictures/Screenshots as
    // Screenshot_<yyyyMMdd-HHmmss>_<app>.png
    async _takeScreenshot(task = this._tasks[this._page()]) {
        const actor = task?.win.get_compositor_private();
        if (!actor)
            return;
        try {
            // the card stands in for the window, which is see-through meanwhile: opaque for this one paint
            const rect = task.win.get_frame_rect(), opacity = actor.opacity;
            let content;
            actor.opacity = 255;
            try {
                content = actor.paint_to_content(rect);
            } finally {
                actor.opacity = opacity;
            }
            const texture = content?.get_texture();
            if (!texture)
                return;
            const dir = GLib.build_filenamev([GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_PICTURES) ??
                GLib.build_filenamev([GLib.get_home_dir(), 'Pictures']), 'Screenshots']);
            GLib.mkdir_with_parents(dir, 0o755);
            const stamp = GLib.DateTime.new_now_local().format('%Y%m%d-%H%M%S');
            const name = (task.app?.get_name() ?? 'App').replace(/[/\\]/g, '');
            this._flash(task);
            // the window paints at its buffer's scale (3 for 2.5): down to the screen's pixels, as Android saves them
            const png = Gio.MemoryOutputStream.new_resizable();
            await Shell.Screenshot.composite_to_stream(texture, 0, 0, -1, -1, 1, null, 0, 0, 1, png);
            png.close(null);
            let pixbuf = GdkPixbuf.Pixbuf.new_from_stream(Gio.MemoryInputStream.new_from_bytes(png.steal_as_bytes()),
                null);
            const w = Math.round(rect.width * this._scale), h = Math.round(rect.height * this._scale);
            if (pixbuf.width !== w || pixbuf.height !== h)
                pixbuf = pixbuf.scale_simple(w, h, GdkPixbuf.InterpType.BILINEAR);
            const file = Gio.File.new_for_path(GLib.build_filenamev([dir, `Screenshot_${stamp}_${name}.png`]));
            const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
            // encoded off the main loop (its finish is the class's, so no promisify)
            await new Promise((resolve, reject) => pixbuf.save_to_streamv_async(stream, 'png', [], [], null,
                (_p, res) => {
                    try {
                        resolve(GdkPixbuf.Pixbuf.save_to_stream_finish(res));
                    } catch (e) {
                        reject(e);
                    }
                }));
            stream.close(null);
        } catch (e) {
            logError(e, 'gnome-phone: recents screenshot');
        }
    }

    // the card flashes white as Android's screenshot does
    _flash(task) {
        const flash = new Clutter.Actor({background_color: cogl(Color.white), opacity: 0});
        const r = task.view.rect.map(v => v / this._scale);
        flash.set_position(r[0], r[1]);
        flash.set_size(r[2] - r[0], r[3] - r[1]);
        this.layer.add_child(flash);
        flash.ease({opacity: 180, duration: 60, onComplete: () =>
            flash.ease({opacity: 0, duration: 200, onComplete: () => flash.destroy()})});
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._stacks?.setOverview(false);
        this._menu = null;
        this._ticker?.stop();
        this._ticker = null;
        this._motions.clear();
        this._nav.watchBack(null);
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        global.display.disconnectObject(this);
        for (const task of this._tasks) {
            if (task.dismissed && !task.lost)
                this._close(task);
            task.win.disconnectObject(this);
            task.view.destroy();
            task.chip.destroy();
        }
        this._tasks = [];
        this._tabs?.destroy();
        this._tabs = null;
        this.layer.destroy();
        this._nav.recentsClosed(this);
    }
}
