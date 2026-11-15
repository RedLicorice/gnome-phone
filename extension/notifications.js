// The notification stack in the shade (spec §4): Main.messageTray's notifications as Android 16's 2025 rows, the
// low-urgency ones under a "Silent" header, the footer with Clear all, and "No notifications" when there is nothing.
// The shade places the stack once per frame (its top, its expansion ratio, the content alpha); rows expand and
// collapse, swipe away and close the gaps they leave on animations of their own.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';
import {FdoNotificationDaemonSource} from 'resource:///org/gnome/shell/ui/notificationDaemon.js';

import {VectorIcon} from './icons.js';
import {animate, animateSpring, Animator, bezier, clamp, FAST_OUT_LINEAR_IN, FAST_OUT_SLOW_IN, flingDismissing,
    lerp, splineFling, Spring, VelocityTracker} from './motion.js';
import {magnetic} from './magnetic.js';
import {RoundRect} from './roundrect.js';
import {bindColor, Color, font, gild, restyleActor, rgba, SurfaceEffect} from './palette.js';
import {owlIcon} from './owl.js';
import {isRenaissance} from './theme.js';
import {openSettings} from './settings.js';
import {symbolicIcon} from './statusbar.js';
import {px, SCALE, SCREEN_H, SCREEN_W} from './units.js';

// --- the stack, screen px (spec §4.1)
export const SIDE = 48;                 // 16 dp each side
export const ROW_W = 984;
const GAP = 6;                          // 2 dp between rows: the scrim shows through
const SECTION_GAP = 48;                 // 16 dp before a new section
const TOP_PADDING = 48;                 // the first row sits 16 dp under the scrim top
const DISMISS_X = 1032;                 // a dismissed row leaves to ±(W − side padding)
const BOTTOM_INSET = 48;                // the stack scrolls to end above the 16 dp navigation handle
const MAX_FLING_VELOCITY = 24000;       // 8000 dp/s

// --- a row (spec §4.3, the 2025 templates); y relative to the row's top
export const MIN_H = 216;               // 72 dp, collapsed
const RADIUS = 84;                      // 28 dp at a section's ends
const RADIUS_SMALL = 12;                // 4 dp elsewhere
const ICON_POS = 48;
const ICON = 120;                       // the 40 dp circle; its glyph is 24 dp
const GLYPH = 72;
const CONTENT_X = 216;                  // 72 dp
const COLLAPSED_END = 820;              // the collapsed text column ends 60 dp before the row's end (x 868)
const EXPANDED_END = 936;               // ... the expanded one 16 dp before it
const DOT_MARGIN = 6;                   // the "•" separator's 2 dp margins
const ONE_LINE_BASELINE = 125;          // collapsed, a title only
const TITLE_BASELINE = 95;              // collapsed, a title and a text line
const TEXT_BASELINE = 154;
const HEADER_BASELINE = 88;             // expanded: app name • time
const BIG_TITLE_BASELINE = 160;
const BIG_TEXT_BASELINE = 245;
const LINE = 60;                        // 20 sp
const MAX_LINES = 8;
const MAX_ACTIONS = 3;
const ACTIONS_TOP = 12;                 // the 48 dp action band starts under the last text line
const ACTIONS_H = 144;
const ACTION_BASELINE = 90;
const ACTION_PAD = 36;                  // 4 dp inset + 8 dp padding on each side of a label
const ACTION_MARGIN = 12;               // 4 dp between buttons
const BOTTOM_WITH_ACTIONS = 175;        // the row's end below its last baseline (measured 420 px for one text line)
const BOTTOM_NO_ACTIONS = 61;           // the text's descent + the action list's 10 dp minimum + 6 dp
const HEADS_UP_ACTIONS_Y = 156;         // a heads-up's actions: under the collapsed layout, 20 dp up into it,
const HEADS_UP_ACTIONS_H = 318;         // ... 72 - 20 + 48 + 6 dp in all (notification_2025_template_heads_up_base)
const PILL_X = 858;                     // the expand pill: 26 x 18 dp, 16 dp in from the top right corner
const PILL_Y = 48;
const PILL_W = 78;
const PILL_H = 54;
const PILL_ICON = 48;                   // 16 sp
const PILL_TOUCH = 48;                  // the button's 16 dp padding around the pill
const PRESS_ALPHA = 0.1;

// --- the "Silent" header and the footer (spec §4.5, §4.6)
const HEADER_H = 144;                   // 48 dp
const HEADER_LABEL_X = 96;              // 16 + 4 + 12 dp
const FOOTER_H = 180;                   // 12 dp padding + the 48 dp buttons
const FOOTER_PILL_Y = 48;               // + the buttons' 4 dp inset
const FOOTER_PILL_H = 120;
const HISTORY_X = 48;
const ICON_BUTTON_W = 180;              // 20 + 20 + 20 dp
const CLEAR_X = 252;
const CLEAR_W = 576;
const SETTINGS_X = 852;
const FOOTER_ICON = 60;                 // 20 dp

// --- motion (spec §4.7-§4.9)
const STANDARD_MS = 360;                // expand/collapse, rows closing a gap, rows coming and going
const ROUNDNESS_MS = 200;
const ESCAPE_MS = 200;                  // a slow release's dismiss, and each of Clear all's slides
const MAX_ESCAPE_MS = 400;
const ESCAPE_VELOCITY = 1500;           // 500 dp/s
const MIN_DISMISS_VELOCITY = 6000;      // 2000 dp/s: a row that let go of its neighbours leaves at least this fast
const MAX_VELOCITY = 12000;             // 4000 dp/s
const FAR_ENOUGH = 0.6;                 // of the row's width: where the content has faded out
const SWIPE_SLOP = 48;                  // the paging touch slop, 16 dp
const SNAP_STIFFNESS = 550;             // the magnetic snap back (SwipeHelper)
const SNAP_DAMPING = 0.52;
const CLEAR_DELAY = 60;                 // Clear all starts the rows bottom-up at 0, 60, 115, 165, ... ms
const CLEAR_DELAY_STEP = 5;
const CLEAR_DELAY_MIN = 30;
const ALPHA_IN = bezier(0.4, 0, 1, 1);  // CrossFadeHelper: what only one layout has fades in its half
const ALPHA_OUT = bezier(0, 0, 0.8, 1);

// fonts (spec §4.4)
const TITLE_FONT = font(14, 600);       // variable-title-small-emphasized
const TEXT_FONT = font(14, 400);        // variable-body-medium
const INFO_FONT = font(12, 400);        // variable-body-small: the app name, "•", the time
const ACTION_FONT = font(14, 600);
const SECTION_FONT = font(14, 600);
const CLEAR_ALL_FONT = font(16, 600);   // variable-title-medium-emphasized
const EMPTY_FONT = font(16, 600);       // TextAppearance.NotificationShadeText

const CAP_HEIGHT = 0.73;                // of the em (spec §4)

const fadeIn = p => ALPHA_IN(clamp((p - 0.5) / 0.5));
const fadeOut = p => 1 - ALPHA_OUT(clamp(p / 0.5));

function makeText(fontName, color, params = {}) {
    return bindColor(new Clutter.Text({font_name: fontName, ...params}), color);
}

// a text's natural width, screen px
function naturalWidth(text) {
    text.width = -1;
    return text.get_preferred_width(-1)[1] * SCALE;
}

// Put a Clutter.Text's first baseline at @baseline and its box at @x (screen px in its parent)
function placeBaseline(text, x, baseline) {
    const b = text.get_layout().get_baseline() / Pango.SCALE / text.get_resource_scale();
    text.set_position(px(x), px(baseline) - b);
}

// The Pango line-height factor that puts the body's lines 20 sp apart
let lineFactor = 0;
function bodyLineFactor(text) {
    if (!lineFactor) {
        text.text = 'Hg';
        const [, logical] = text.get_layout().get_pixel_extents();
        lineFactor = LINE / (logical.height / text.get_resource_scale() * SCALE);
    }
    return lineFactor.toFixed(4);
}

// The lines @markup wraps to at @width (screen px), on a layout of its own: the text's may be cut to its old height
function countLines(text, markup, width) {
    const layout = text.create_pango_layout(null);
    layout.set_font_description(Pango.FontDescription.from_string(TEXT_FONT));
    layout.set_wrap(Pango.WrapMode.WORD_CHAR);
    layout.set_width(Math.round(px(width) * Pango.SCALE));
    layout.set_markup(markup, -1);
    return layout.get_line_count();
}

// DateTimeView's short relative time
function shortTime(datetime) {
    if (!datetime)
        return '';
    const s = GLib.DateTime.new_now_local().difference(datetime) / 1e6;
    if (s < 60)
        return 'now';
    if (s < 3600)
        return `${Math.floor(s / 60)}m`;
    if (s < 86400)
        return `${Math.floor(s / 3600)}h`;
    if (s < 365 * 86400)
        return `${Math.floor(s / 86400)}d`;
    return `${Math.floor(s / (365 * 86400))}y`;
}

// Chronometer.updateText: whole seconds since @base (Unix ms), or left until it, as DateUtils.formatElapsedTime;
// a count-down gone past zero gets a minus
function chronometer(base, countDown) {
    const now = GLib.get_real_time() / 1000;
    let s = Math.trunc((countDown ? base - now : now - base) / 1000);
    const minus = s < 0 ? '\u2212' : '';
    s = Math.abs(s);
    const mm = String(Math.floor(s / 60) % 60).padStart(2, '0');
    const ss = String(s % 60).padStart(2, '0');
    return s >= 3600 ? `${minus}${Math.floor(s / 3600)}:${mm}:${ss}` : `${minus}${mm}:${ss}`;
}

// what follows the "•" in the header: the chronometer, the sub-text, both, or else how long ago it came
function headerInfo(n) {
    const parts = [];
    if (n.neoSubtext)
        parts.push(n.neoSubtext);
    if (n.neoChronometer)
        parts.push(chronometer(n.neoChronometer, n.neoCountDown));
    return parts.length ? parts.join(' \u2022 ') : shortTime(n.datetime);
}

// Android's header extras (setUsesChronometer, setChronometerCountDown, setSubText), which
// org.freedesktop.Notifications has no place for: the Clock sends them as hints (apps/clock/src/clock-notifier.h),
// and the shell's daemon keeps only the hints it knows. Its Notify unpacks the hints in place and then hands the notification to its source, so
// the source's processNotification can still read them, before a row is made for a new notification and before an
// old one's row updates (on an idle). Returns the undo.
function keepHeaderHints() {
    const daemon = Main.notificationDaemon?._fdoNotificationDaemon;
    if (!daemon)
        return () => {};
    const notify = daemon.NotifyAsync;
    daemon.NotifyAsync = function (params, invocation) {
        this._neoHints = params[6];
        try {
            return notify.call(this, params, invocation);
        } finally {
            this._neoHints = null;
        }
    };
    const proto = FdoNotificationDaemonSource.prototype;
    const process = proto.processNotification;
    proto.processNotification = function (notification, ...args) {
        const hints = daemon._neoHints;
        if (hints) {
            notification.neoChronometer = Number(hints['x-neo-chronometer'] ?? 0);
            notification.neoCountDown = !!hints['x-neo-chronometer-countdown'];
            notification.neoSubtext = hints['x-neo-subtext'] ?? '';
        }
        return process.call(this, notification, ...args);
    };
    return () => {
        delete daemon.NotifyAsync;
        if (daemon.NotifyAsync !== notify)
            daemon.NotifyAsync = notify;
        proto.processNotification = process;
    };
}

function plainBody(notification) {
    let body = notification.body ?? '';
    if (notification.useBodyMarkup) {
        try {
            [, , body] = Pango.parse_markup(body, -1, '');
        } catch {
            body = body.replace(/<[^>]*>/g, '');
        }
    }
    return body.trim();
}

// --- a notification row

// @stack takes the row's taps and swipes; a heads-up has a card of its own colour (@card [rgb, alpha]), and the
// circle's glyph is punched out in it (@glyph)
export const Row = GObject.registerClass(
class Row extends St.Widget {
    constructor(stack, notification, {card = SurfaceEffect[1], glyph = Color.glyph} = {}) {
        super({reactive: true, width: px(ROW_W), height: px(MIN_H)});
        this._stack = stack;
        this.notification = notification;
        this._cardColor = card;
        this._glyphColor = glyph;
        this.headsUp = false;
        this.userExpanded = null;       // the user's choice; else the stack opens the first alerting row
        this.expanded = false;
        this.expandP = 0;               // as laid out: 0 collapsed .. 1 expanded
        this.tx = 0;                    // the swipe's translation, px
        this._fadeTx = 0;               // ... the part the content fades by: the row's own swipe, not a neighbour's
        this.dismissing = false;
        this._roundness = [0, 0];       // top, bottom: 0 is 4 dp, 1 is 28 dp
        this._baseRound = [0, 0];       // ... the row's own (a section's ends are round)
        this._swipeRound = [0, 0];      // ... what a swipe asks for; the larger of the two shows
        this._visibleTop = 0;
        this._visibleH = MIN_H;
        this._contentAlpha = 1;
        this._velocity = new VelocityTracker();

        // the card is the button that opens the notification; the content lies over it and takes no touches
        this._card = new St.Button({
            reactive: true,
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            width: px(ROW_W),
            height: px(MIN_H),
        });
        // its fill a shader's: a swipe changes the corners every frame, which St would draw anew each time
        this._shape = new RoundRect();
        this._card.set_child(this._shape);
        this._card.connect('notify::pressed', () => this._applyCard());
        this._card.connect('clicked', () => this._stack.activate(this));
        this.add_child(this._card);

        this._content = new St.Widget({width: px(ROW_W), height: px(MIN_H)});
        this.add_child(this._content);
        this._iconBox = new St.Widget({x: px(ICON_POS), y: px(ICON_POS), width: px(ICON), height: px(ICON)});
        this._content.add_child(this._iconBox);

        const ellipsize = Pango.EllipsizeMode.END;
        this._title = makeText(TITLE_FONT, Color.onSurface, {ellipsize});
        this._app = makeText(INFO_FONT, Color.onSurface, {ellipsize});
        this._dot = makeText(INFO_FONT, Color.onSurface, {text: '•'});
        this._time = makeText(INFO_FONT, Color.onSurface);
        this._text = makeText(TEXT_FONT, Color.onSurface, {ellipsize});
        this._bigText = makeText(TEXT_FONT, Color.onSurface, {
            ellipsize, line_wrap: true, line_wrap_mode: Pango.WrapMode.WORD_CHAR,
        });
        for (const text of [this._title, this._app, this._dot, this._time, this._text, this._bigText])
            this._content.add_child(text);
        this._dotW = naturalWidth(this._dot);

        this._actions = new St.Widget({visible: false});
        this._actionButtons = [];
        this._content.add_child(this._actions);

        // the expand pill and its 16 dp of touch padding; the chevron swaps without animation
        this._pill = new St.Button({
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            accessible_name: 'Expand',
            width: px(PILL_W + 2 * PILL_TOUCH),
            height: px(PILL_H + 2 * PILL_TOUCH),
        });
        const pillBox = new St.Widget({width: px(PILL_W + 2 * PILL_TOUCH), height: px(PILL_H + 2 * PILL_TOUCH)});
        // surface_effect_3 flattened on the card (measured)
        const pillStyle = () => `background-color: ${rgba(Color.pill)}; border-radius: ${px(PILL_H / 2).toFixed(2)}px;${gild()}`;
        const pillBg = new St.Widget({
            x: px(PILL_TOUCH), y: px(PILL_TOUCH), width: px(PILL_W), height: px(PILL_H),
            style: pillStyle(),
        });
        this._chevron = new VectorIcon('ic_notification_2025_expand', px(PILL_ICON), Color.onSurface);
        this._chevron.set_position(px((PILL_W - PILL_ICON) / 2), px((PILL_H - PILL_ICON) / 2));
        pillBg.add_child(this._chevron);
        this._pillBg = pillBg;
        pillBox.add_child(pillBg);
        this._pill.set_child(pillBox);
        this._pill.connect('clicked', () => this._stack.toggleExpanded(this));
        this._content.add_child(this._pill);

        // the look is live: what was copied into styles and fills follows (texts and icons are bound or Canvases)
        restyleActor(this, () => {
            this._pillBg.style = pillStyle();
            this._iconKey = null;       // the circle's colours are in its style
            this._updateIcon();
            this._applyCard();
        });

        // swiped sideways it goes (SwipeHelper, the magnetic path: magnetic.js)
        const swipe = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.X,
            max_n_points: 1,
            begin_threshold: px(SWIPE_SLOP),
        });
        swipe.connect('recognize', g => this._beginSwipe(g));
        swipe.connect('pan-update', g => this._updateSwipe(g));
        swipe.connect('end', () => this._endSwipe(false));
        swipe.connect('cancel', () => this._endSwipe(true));
        this.add_action(swipe);

        // a chronometer ticks while the row is on screen
        this._tickId = 0;
        this.connect('notify::mapped', () => this._setTime(false));
        this.connect('destroy', () => {
            this._swipeAnim?.stop();
            this._magneticAnim?.stop();
            this._roundAnim?.stop();
            if (this._tickId)
                GLib.source_remove(this._tickId);
        });

        this._applyCard();
        this.update();
    }

    get silent() {
        return this._urgency === MessageTray.Urgency.LOW;
    }

    get critical() {
        return this._urgency === MessageTray.Urgency.CRITICAL;
    }

    // the notification is gone: nothing may touch it any more
    forget() {
        this.notification = null;
    }

    // --- content

    update() {
        const n = this.notification;
        if (!n)
            return;
        this._urgency = n.urgency;
        // a notification without a title shows its text in the title's place
        let title = (n.title ?? '').trim();
        let body = plainBody(n);
        if (!title)
            [title, body] = [body, ''];
        this._title.text = title.replace(/\s*\n\s*/g, ' ');
        this._titleW = naturalWidth(this._title);
        this._app.text = n.source?.title ?? '';
        this._appW = naturalWidth(this._app);

        this._hasText = body.length > 0;
        this._text.text = body.replace(/\s*\n\s*/g, ' ');
        const markup = `<span line_height="${bodyLineFactor(this._bigText)}">` +
            `${GLib.markup_escape_text(body, -1)}</span>`;
        this._bigText.set_markup(markup);
        this._bigText.set_size(px(EXPANDED_END - CONTENT_X), -1);
        this._bigText.remove_clip();
        this._lines = this._hasText ? countLines(this._bigText, markup, EXPANDED_END - CONTENT_X) : 0;
        if (this._lines > MAX_LINES) {
            // a fixed height ends the wrapped text in an ellipsis at that line
            this._bigText.height = px(MAX_LINES * LINE);
            this._bigText.set_clip(0, 0, px(EXPANDED_END - CONTENT_X), px(MAX_LINES * LINE));
            this._lines = MAX_LINES;
        }
        // the collapsed line is cut short: the expanded text cross-fades in over it
        this._crossFade = this._lines > 1 || naturalWidth(this._text) > COLLAPSED_END - CONTENT_X;
        this._text.width = px(COLLAPSED_END - CONTENT_X);

        this._updateIcon();
        this._updateActions();

        const last = this._lines > 0 ? BIG_TEXT_BASELINE + (this._lines - 1) * LINE : BIG_TITLE_BASELINE;
        this._lastBaseline = last;
        this.expandedHeight = Math.max(MIN_H,
            last + (this._actionButtons.length > 0 ? BOTTOM_WITH_ACTIONS : BOTTOM_NO_ACTIONS));
        this._setTime();
    }

    // every row's time is looked at as the shade shows, in its first frame, and most say what they said
    updateTime() {
        if (this.notification && headerInfo(this.notification) !== this._time.text)
            this._setTime();
    }

    // @relayout: the rest of the row changed too (update); else only a different time moves anything
    _setTime(relayout = true) {
        const n = this.notification;
        if (!n)
            return;
        const text = headerInfo(n);
        if (relayout || text !== this._time.text) {
            this._time.text = text;
            this._timeW = naturalWidth(this._time);
            this._layoutContent();
        }
        // the next whole second from the chronometer's base, where its text changes
        if (n.neoChronometer && this.mapped && !this._tickId) {
            const phase = ((GLib.get_real_time() / 1000 - n.neoChronometer) % 1000 + 1000) % 1000;
            this._tickId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.ceil(1000 - phase) + 1, () => {
                this._tickId = 0;
                this._setTime(false);
                return GLib.SOURCE_REMOVE;
            });
        }
    }

    // the app's own icon for an app (notificationsRedesignAppIcons); else the small icon in a primary circle,
    // punched out in the card's colour
    _updateIcon() {
        const n = this.notification;
        const source = n.source;
        const app = source?.app ?? source?._app ?? null;
        const gicon = n.gicon ?? source?.icon ?? null;
        // the Minerva agent's notifications carry the owl alone in the Renaissance looks
        const owl = isRenaissance() && /minerva/i.test(app?.get_id() ?? source?.title ?? '');
        const key = (owl ? 'owl:' : '') + (app ? `app:${app.get_id()}` : gicon?.to_string() ?? '');
        if (key === this._iconKey)
            return;
        this._iconKey = key;
        this._iconBox.destroy_all_children();
        if (owl) {
            this._iconBox.add_child(owlIcon(px(ICON)));
            return;
        }

        const symbolic = app ? null : symbolicIcon(gicon ?? new Gio.ThemedIcon({name: 'dialog-information'}));
        if (symbolic) {
            const circle = new St.Widget({
                width: px(ICON), height: px(ICON),
                style: `background-color: ${rgba(Color.primary)}; border-radius: ${px(ICON / 2).toFixed(2)}px;`,
            });
            const glyph = new St.Icon({
                gicon: symbolic,
                icon_size: Math.round(px(GLYPH)),
                // every class of a symbolic icon in the one colour
                style: ['color', 'warning-color', 'error-color', 'success-color']
                    .map(p => `${p}: ${rgba(this._glyphColor)};`).join(' '),
            });
            glyph.set_position((px(ICON) - glyph.icon_size) / 2, (px(ICON) - glyph.icon_size) / 2);
            circle.add_child(glyph);
            this._iconBox.add_child(circle);
        } else {
            this._iconBox.add_child(new St.Icon({
                gicon: app ? app.get_icon() : gicon,
                icon_size: Math.round(px(ICON)),
            }));
        }
    }

    // borderless buttons from x 56 dp, labels in primary
    _updateActions() {
        const actions = this.notification.actions.slice(0, MAX_ACTIONS);
        const labels = actions.map(a => a.label).join('\n');
        if (labels === this._actionLabels)
            return;
        this._actionLabels = labels;
        this._actions.destroy_all_children();
        this._actionButtons = [];
        let x = CONTENT_X;
        for (const action of actions) {
            const label = makeText(ACTION_FONT, Color.primary, {text: action.label});
            const w = naturalWidth(label);
            const button = new St.Button({
                x: px(x - ACTION_PAD), y: 0,
                width: px(w + 2 * ACTION_PAD), height: px(ACTIONS_H),
                can_focus: true,
                button_mask: St.ButtonMask.PRIMARY,
                accessible_name: action.label,
            });
            const box = new St.Widget({width: px(w + 2 * ACTION_PAD), height: px(ACTIONS_H)});
            box.add_child(label);
            placeBaseline(label, ACTION_PAD, ACTION_BASELINE);
            button.set_child(box);
            button.connect('clicked', () => this._stack.activateAction(this, action));
            this._actions.add_child(button);
            this._actionButtons.push(button);
            x += w + 2 * ACTION_PAD + ACTION_MARGIN;
        }
    }

    // collapsed, a heads-up has its actions under the content
    get collapsedHeight() {
        return this.headsUp && this._actionButtons.length > 0 ? HEADS_UP_ACTIONS_H : MIN_H;
    }

    heightFor(p) {
        return lerp(this.collapsedHeight, this.expandedHeight, p);
    }

    setHeadsUp(headsUp) {
        this.headsUp = headsUp;
        this._layoutContent();
    }

    setChevron(expanded) {
        this._chevron.name = expanded ? 'ic_notification_2025_collapse' : 'ic_notification_2025_expand';
        this._pill.accessible_name = expanded ? 'Collapse' : 'Expand';
    }

    setExpansion(p) {
        if (p === this.expandP)
            return;
        this.expandP = p;
        this._layoutContent();
    }

    // Collapsed and expanded layouts cross-transform with the height: the title, the text and "• time" move in
    // place, the app name, the actions and a longer text fade in (ViewTransformationHelper, TransformState)
    _layoutContent() {
        if (this._timeW === undefined)
            return;
        const p = this.expandP;
        const infoW = 2 * DOT_MARGIN + this._dotW + this._timeW;
        const collapsedBaseline = this._hasText ? TITLE_BASELINE : ONE_LINE_BASELINE;

        const titleW = lerp(Math.min(this._titleW, COLLAPSED_END - CONTENT_X - infoW),
            Math.min(this._titleW, EXPANDED_END - CONTENT_X), p);
        this._title.width = px(Math.max(titleW, 0));
        placeBaseline(this._title, CONTENT_X, lerp(collapsedBaseline, BIG_TITLE_BASELINE, p));

        // "• time": behind the title collapsed, behind the app name expanded
        const appW = Math.min(this._appW, EXPANDED_END - CONTENT_X - infoW);
        this._app.width = px(Math.max(appW, 0));
        placeBaseline(this._app, CONTENT_X, HEADER_BASELINE);
        this._app.opacity = Math.round(255 * fadeIn(p));
        const collapsedInfoX = CONTENT_X + Math.min(this._titleW, COLLAPSED_END - CONTENT_X - infoW);
        const infoX = lerp(collapsedInfoX, CONTENT_X + appW, p) + DOT_MARGIN;
        const infoBaseline = lerp(collapsedBaseline, HEADER_BASELINE, p);
        placeBaseline(this._dot, infoX, infoBaseline);
        placeBaseline(this._time, infoX + this._dotW + DOT_MARGIN, infoBaseline);

        this._text.visible = this._hasText;
        this._bigText.visible = this._hasText && this._crossFade && p > 0;
        if (this._hasText) {
            const baseline = lerp(TEXT_BASELINE, BIG_TEXT_BASELINE, p);
            placeBaseline(this._text, CONTENT_X, baseline);
            this._text.opacity = this._crossFade ? Math.round(255 * (1 - p)) : 255;
            placeBaseline(this._bigText, CONTENT_X, baseline);
            this._bigText.opacity = Math.round(255 * p);
        }

        // the actions sit where the expanded layout has them; the growing row uncovers them. A heads-up shows them
        // under its collapsed layout, and they move down to there as it expands
        const headsUpActions = this.headsUp && this._actionButtons.length > 0;
        const actionsY = this._lastBaseline + ACTIONS_TOP;
        this._actions.visible = (p > 0 || headsUpActions) && this._actionButtons.length > 0;
        this._actions.set_position(0, px(headsUpActions ? lerp(HEADS_UP_ACTIONS_Y, actionsY, p) : actionsY));
        this._actions.opacity = headsUpActions ? 255 : Math.round(255 * fadeIn(p));

        // the pill: centred in a one-line row, at the top otherwise
        const pillY = this._hasText ? PILL_Y : lerp((MIN_H - PILL_H) / 2, PILL_Y, p);
        this._pill.set_position(px(PILL_X - PILL_TOUCH), px(pillY - PILL_TOUCH));
    }

    // --- the card: its height (the stack's clip) and corners

    setVisibleHeight(h) {
        this.setVisibleRange(0, h);
    }

    // the card from @top to @bottom of the layout: a heads-up cycling out is cut from the top
    setVisibleRange(top, bottom) {
        if (Math.abs(top - this._visibleTop) < 0.01 && Math.abs(bottom - this._visibleH) < 0.01)
            return;
        this._visibleTop = top;
        this._visibleH = bottom;
        this.height = px(bottom);
        this._card.set_position(0, px(top));
        this._card.set_size(px(ROW_W), px(bottom - top));
        this._shape.set_size(px(ROW_W), px(bottom - top));
        this._content.set_clip(0, px(top), px(ROW_W), px(bottom - top));
    }

    // the content's alpha in a heads-up's appear animation (ActivatableNotificationView.setContentAlpha)
    setContentAlpha(alpha) {
        this._contentAlpha = alpha;
        this._applyContentAlpha();
    }

    // the row's own corners
    setBaseRoundness(top, bottom, animated = false) {
        this._baseRound = [top, bottom];
        this._syncRoundness(animated);
    }

    // the corners a swipe asks for (NotificationRoundnessManager's DISMISS_ANIMATION source); 0, 0 lets go
    setSwipeRoundness(top, bottom, animated = false) {
        this._swipeRound = [top, bottom];
        this._syncRoundness(animated);
    }

    _syncRoundness(animated) {
        this.setRoundness(Math.max(this._baseRound[0], this._swipeRound[0]),
            Math.max(this._baseRound[1], this._swipeRound[1]), animated);
    }

    setRoundness(top, bottom, animated = false) {
        const target = this._roundTarget;
        if (target && target[0] === top && target[1] === bottom)
            return;
        this._roundTarget = [top, bottom];
        this._roundAnim?.stop();
        this._roundAnim = null;
        if (!animated || !this.mapped) {
            this._roundness = [top, bottom];
            this._applyCard();
            return;
        }
        const [top0, bottom0] = this._roundness;
        this._roundAnim = animate(this, {
            from: 0, to: 1, duration: ROUNDNESS_MS, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._roundness = [lerp(top0, top, v), lerp(bottom0, bottom, v)];
                this._applyCard();
            },
            onDone: () => {
                this._roundAnim = null;
            },
        });
    }

    _applyCard() {
        const [top, bottom] = this._roundness;
        const rt = px(lerp(RADIUS_SMALL, RADIUS, top)), rb = px(lerp(RADIUS_SMALL, RADIUS, bottom));
        this._shape.setRadii(rt, rt, rb, rb);
        // pressed: the press highlight over the card, as one colour
        const [rgb, alpha] = this._cardColor;
        if (!this._card.pressed) {
            this._shape.setColor(...rgb, alpha);
            return;
        }
        const a = PRESS_ALPHA + alpha * (1 - PRESS_ALPHA);
        const mix = (c, i) => (Color.onSurface[i] * PRESS_ALPHA + c * alpha * (1 - PRESS_ALPHA)) / a;
        this._shape.setColor(...rgb.map(mix), a);
    }

    // --- swipe to dismiss

    get swiping() {
        return !!this._swiping;
    }

    _trackVelocity(gesture) {
        const event = gesture.get_point_event(0);
        const time = event ? event.get_time() : GLib.get_monotonic_time() / 1000;
        this._velocity.add(time, gesture.get_centroid_abs().x * SCALE, 0);
    }

    _beginSwipe(gesture) {
        if (this.dismissing)
            return;
        this._swipeAnim?.stop();
        this._swipeAnim = null;
        this._swiping = true;
        this._swipeStartX = gesture.get_centroid_abs().x;
        this._swipeStartTx = this.tx;
        this._velocity.clear();
        this._trackVelocity(gesture);
        magnetic.setTargets(this, this._stack.magneticRows?.(this));
        this._stack.onSwipeBegin(this);
    }

    _updateSwipe(gesture) {
        if (!this._swiping)
            return;
        this._trackVelocity(gesture);
        const t = this._swipeStartTx + (gesture.get_centroid_abs().x - this._swipeStartX) * SCALE;
        if (!magnetic.setTranslation(this, t))
            this.setTranslation(t);
    }

    _endSwipe(cancelled) {
        if (!this._swiping)
            return;
        const v = cancelled ? 0 : clamp(this._velocity.velocityX, -MAX_VELOCITY, MAX_VELOCITY);
        if (!cancelled && magnetic.isDismissible(this, v)) {
            // SwipeHelper.dismissChild: one that let go leaves the way it went, at 2000 dp/s at least
            const fast = Math.abs(v) > ESCAPE_VELOCITY && (v > 0) === (this.tx > 0);
            const direction = magnetic.detachDirection(this);
            const out = direction !== 0 ? direction * Math.max(MIN_DISMISS_VELOCITY, Math.abs(v)) : v;
            this._swiping = false;
            magnetic.end(this, true, out);
            this.dismiss(out, !fast);
        } else {
            this._swiping = false;
            magnetic.end(this, false, v);
            this._stack.onSwipeEnd(this);
            this._snapBack(v);
        }
    }

    // --- the magnetic swipe's listener (ExpandableView.mMagneticRowListener)

    get canBeDismissed() {
        return !this.dismissing;
    }

    // the finger's speed while this row is the one swiped, px/s
    get swipeVelocity() {
        return this._swiping ? this._velocity.velocityX : 0;
    }

    // to @t at once, or, while a magnetic spring runs, by that spring
    setMagneticTranslation(t) {
        if (this._magneticAnim)
            this._magneticSpring.target = t;
        else
            this.setTranslation(t, this._swiping);
    }

    triggerMagneticForce(t, {stiffness, dampingRatio}, v = 0) {
        this.cancelTranslationAnimations();
        this._magneticAnim?.stop();
        // SpringForce's thresholds for a translation: 0.75 px, 46.9 px/s
        this._magneticSpring = new Spring({stiffness, dampingRatio, value: this.tx, velocity: v, target: t,
            threshold: 0.75});
        const follow = x => this.setTranslation(x, this._swiping);
        this._magneticAnim = animateSpring(this, this._magneticSpring, follow, () => {
            this._magneticAnim = null;
        });
    }

    cancelMagneticAnimations() {
        this._magneticAnim?.stop();
        this._magneticAnim = null;
    }

    cancelTranslationAnimations() {
        this._swipeAnim?.stop();
        this._swipeAnim = null;
    }

    // the content fades as the row goes, the card does not (SwipeHelper.updateSwipeProgressFromOffset); not for a
    // neighbour a swipe pulls along (@fade false), which SwipeHelper never touches
    setTranslation(tx, fade = true) {
        this.tx = tx;
        this.translation_x = px(tx);
        if (fade || this._fadeTx !== 0) {
            this._fadeTx = fade ? tx : 0;
            this._applyContentAlpha();
        }
    }

    _applyContentAlpha() {
        this._content.opacity = Math.round(255 * this._contentAlpha *
            (1 - clamp(Math.abs(this._fadeTx) / ROW_W / FAR_ENOUGH)));
    }

    // SwipeHelper.dismissChild: a slow release accelerates out, a fling keeps its speed
    dismiss(v, accelerate, {delay = 0, onDone = null} = {}) {
        this.dismissing = true;
        const target = (Math.abs(v) > ESCAPE_VELOCITY && v < 0) || this.tx < 0 ? -DISMISS_X : DISMISS_X;
        let duration, interpolator;
        if (accelerate) {
            duration = v !== 0 ? Math.min(MAX_ESCAPE_MS, Math.abs(target - this.tx) * 1000 / Math.abs(v))
                : ESCAPE_MS;
            interpolator = FAST_OUT_LINEAR_IN;
        } else {
            ({duration, interpolator} = flingDismissing(this.tx, target, v, ROW_W));
        }
        this._swipeAnim?.stop();
        this._magneticAnim?.stop();
        this._magneticAnim = null;
        this._swipeAnim = animate(this, {
            from: this.tx, to: target, duration, interpolator, delay,
            onUpdate: x => this.setTranslation(x),
            onDone: () => {
                this._swipeAnim = null;
                // the swipe's corners go (handleChildViewDismissed)
                magnetic.resetRoundness();
                if (onDone)
                    onDone();
                else
                    this._stack.onDismissed(this);
            },
        });
    }

    // the corners go back as it first swings past where it rests (onChildSnapBackOvershoots)
    _snapBack(v) {
        const spring = new Spring({
            stiffness: SNAP_STIFFNESS, dampingRatio: SNAP_DAMPING, value: this.tx, velocity: v, target: 0,
            threshold: 0.75,
        });
        let side = Math.sign(this.tx);
        this._swipeAnim?.stop();
        this._magneticAnim?.stop();
        this._magneticAnim = null;
        this._swipeAnim = animateSpring(this, spring, x => {
            this.setTranslation(x);
            if (side !== 0 && Math.sign(x) === -side) {
                side = 0;
                magnetic.resetRoundness();
            }
        }, () => {
            this._swipeAnim = null;
            if (side !== 0)
                magnetic.resetRoundness();
        });
    }

    // the shade went away mid-swipe
    resetSwipe() {
        if (this.dismissing)
            return;
        this._swiping = false;
        this._swipeAnim?.stop();
        this._swipeAnim = null;
        this.cancelMagneticAnimations();
        this.setSwipeRoundness(0, 0);
        this.setTranslation(0);
    }
});

// --- "Silent"

const SectionHeader = GObject.registerClass(
class SectionHeader extends St.Widget {
    constructor() {
        super({width: px(SCREEN_W), height: px(HEADER_H)});
        this._label = makeText(SECTION_FONT, Color.onSurface, {text: 'Silent'});
        this.add_child(this._label);
        // the cap height centred in the 48 dp
        placeBaseline(this._label, HEADER_LABEL_X, HEADER_H / 2 + CAP_HEIGHT * 14 * 3 / 2);
    }

    setVisibleHeight(h) {
        this.set_clip(0, 0, px(SCREEN_W), px(h));
    }
});

// --- history, Clear all, settings (NotifRedesignFooter)

const Footer = GObject.registerClass(
class Footer extends St.Widget {
    constructor(stack) {
        super({width: px(SCREEN_W), height: px(FOOTER_H)});
        const history = this._button(HISTORY_X, ICON_BUTTON_W, 'Notification history',
            new VectorIcon('notif_footer_btn_history', px(FOOTER_ICON), Color.onSurface));
        // a Clutter.Text right inside a button would take the button's theme font
        const label = new St.Widget({layout_manager: new Clutter.BinLayout()});
        label.add_child(makeText(CLEAR_ALL_FONT, Color.onSurface, {text: 'Clear all'}));
        const clear = this._button(CLEAR_X, CLEAR_W, 'Clear all', label);
        const settings = this._button(SETTINGS_X, ICON_BUTTON_W, 'Notification settings',
            new VectorIcon('notif_footer_btn_settings', px(FOOTER_ICON), Color.onSurface));
        // this phone keeps no notification history: both icon buttons open the notification settings
        history.connect('clicked', () => stack.openSettings());
        settings.connect('clicked', () => stack.openSettings());
        clear.connect('clicked', () => stack.clearAll());
    }

    // 40 dp pills, surface_effect_1 with blur
    _button(x, w, name, child) {
        const button = new St.Button({
            x: px(x), y: px(FOOTER_PILL_Y), width: px(w), height: px(FOOTER_PILL_H),
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            accessible_name: name,
        });
        const style = () => `background-color: ${rgba(...SurfaceEffect[1])}; ` +
            `border-radius: ${px(FOOTER_PILL_H / 2).toFixed(2)}px;`;
        button.style = style();
        restyleActor(button, () => {
            button.style = style();
        });
        // centred at its own size, not stretched over the pill
        child.x_align = Clutter.ActorAlign.CENTER;
        child.y_align = Clutter.ActorAlign.CENTER;
        button.set_child(child);
        this.add_child(button);
        return button;
    }

    setVisibleHeight(h) {
        this.set_clip(0, 0, px(SCREEN_W), px(h));
    }
});

// --- the stack

export const NotificationStack = GObject.registerClass(
class NotificationStack extends St.Widget {
    constructor(shade) {
        super({width: px(SCREEN_W), height: px(SCREEN_H)});
        this._shade = shade;
        this._rows = new Map();         // Notification -> Row
        this._items = [];               // laid out top to bottom: rows, "Silent", the footer
        this._sections = [];            // the alerting rows and the silent ones, each in order
        this._layout = new Map();       // actor -> {offset, h, alpha, p} as shown, and the transition's ends
        this._order = 0;
        this._stackY = 0;
        this._ratio = 1;
        this._scroll = 0;
        this._clearing = false;

        this._header = new SectionHeader();
        this._header.visible = false;
        this.add_child(this._header);
        this._footer = new Footer(this);
        this._footer.visible = false;
        this.add_child(this._footer);
        this._empty = makeText(EMPTY_FONT, Color.onSurface, {text: 'No notifications'});
        this.add_child(this._empty);
        this._emptyAlpha = {now: 1, from: 1, to: 1};

        Main.messageTray.connectObject(
            'source-added', (_tray, source) => this._addSource(source),
            'source-removed', (_tray, source) => this._removeSource(source),
            this);
        for (const source of Main.messageTray.getSources())
            this._addSource(source);
        shade.connectObject('notify::visible', () => this._shadeVisibleChanged(), this);
        this._undoHints = keepHeaderHints();
        this.connect('destroy', () => this._onDestroy());
        this._relayout(false);
    }

    get isEmpty() {
        return this._rows.size === 0;
    }

    // stackY: the scrim top (screen px); ratio: the stack's expansion without the shelf
    // (StackScrollAlgorithm.getExpansionFractionWithoutShelf); alpha: the content alpha
    layoutFor({stackY, ratio, alpha}) {
        this._stackY = stackY;
        this._ratio = ratio;
        this.opacity = Math.round(255 * alpha);
        this._place();
    }

    // --- scrolling, when the stack is longer than the screen under the QQS; the shade hands its drags over

    // from the heights as shown, so a row leaving a scrolled stack does not jump it
    get _maxScroll() {
        let height = 0;
        for (const state of this._layout.values()) {
            if (!state.leaving)
                height = Math.max(height, state.offset + state.h);
        }
        const bottom = this._stackY + TOP_PADDING + height;
        return Math.max(0, bottom - (SCREEN_H - BOTTOM_INSET));
    }

    // a drag up (or down) at screen y would scroll the stack
    canScroll(y, up) {
        if (y < this._stackY)
            return false;
        return up ? this._scroll < this._maxScroll : this._scroll > 0;
    }

    beginScroll() {
        this._scrollAnim?.stop();
        this._scrollAnim = null;
        this._scrollStart = this._scroll;
    }

    dragScroll(dy) {
        this._setScroll(this._scrollStart - dy);
    }

    // OverScroller.fling with the finger's velocity; it stops at the ends
    flingScroll(velocity) {
        const {distance, duration, position} = splineFling(-clamp(velocity, -MAX_FLING_VELOCITY, MAX_FLING_VELOCITY));
        const from = this._scroll, max = this._maxScroll;
        if (duration <= 0)
            return;
        this._scrollAnim = new Animator(this, (dt, elapsed) => {
            const t = Math.min(elapsed * 1000 / duration, 1);
            this._setScroll(from + distance * position(t));
            return t < 1 && this._scroll > 0 && this._scroll < max;
        }, () => {
            this._scrollAnim = null;
        });
    }

    stopScroll() {
        this._scrollAnim?.stop();
        this._scrollAnim = null;
    }

    _setScroll(scroll) {
        scroll = clamp(scroll, 0, this._maxScroll);
        if (scroll === this._scroll)
            return;
        this._scroll = scroll;
        this._place();
    }

    // --- the notifications

    _addSource(source) {
        source.connectObject(
            'notification-added', (_s, n) => this._addNotification(n),
            'notification-removed', (_s, n) => this._removeNotification(n),
            this);
        for (const n of source.notifications)
            this._addNotification(n);
    }

    _removeSource(source) {
        source.disconnectObject(this);
    }

    _addNotification(notification) {
        if (this._rows.has(notification))
            return;
        const row = new Row(this, notification);
        row.order = ++this._order;
        this._rows.set(notification, row);
        this.insert_child_below(row, this._header);
        notification.connectObject(
            'notify', (_n, pspec) => {
                if (pspec.name !== 'acknowledged')
                    this._queueUpdate(row);
            },
            'action-added', () => this._queueUpdate(row),
            'action-removed', () => this._queueUpdate(row),
            this);
        this._relayout(this._shade.visible);
    }

    _queueUpdate(row) {
        row.updatePending = true;
        if (this._updateId)
            return;
        this._updateId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._updateId = 0;
            for (const r of this._rows.values()) {
                if (r.updatePending) {
                    r.updatePending = false;
                    r.update();
                }
            }
            this._relayout(this._shade.visible);
            return GLib.SOURCE_REMOVE;
        });
    }

    _removeNotification(notification) {
        const row = this._rows.get(notification);
        if (!row)
            return;
        this._rows.delete(notification);
        notification.disconnectObject(this);
        row.forget();
        const state = this._layout.get(row);
        // a row swiped (or cleared) out is gone already; another one fades where it is
        if (row.dismissing || !state || !this._shade.visible) {
            this._layout.delete(row);
            row.destroy();
        } else {
            state.leaving = true;
        }
        this._relayout(this._shade.visible && !this._clearing);
    }

    activate(row) {
        if (this._busy(row))
            return;
        const n = row.notification;
        this._shade.close();
        n.activate();
    }

    activateAction(row, action) {
        if (this._busy(row))
            return;
        this._shade.close();
        action.activate();
    }

    _busy(row) {
        return !row.notification || row.dismissing || row.swiping || this._clearing;
    }

    // a notification's row is shown expanded
    rowExpanded(notification) {
        return this._rows.get(notification)?.expanded ?? false;
    }

    toggleExpanded(row) {
        if (this._busy(row))
            return;
        row.userExpanded = !row.expanded;
        this._relayout(true);
    }

    openSettings() {
        this._shade.close();
        openSettings('notifications');
    }

    // --- layout

    // Alerting rows first, the low-urgency ones under "Silent", then the footer; newest first, critical ones on top.
    // Each item's offset below the first row's top is what the shade's ratio squeezes; a section's ends are round.
    _relayout(animated) {
        const rows = [...this._rows.values()].sort((a, b) =>
            (b.critical - a.critical) || (b.order - a.order));
        const alerting = rows.filter(r => !r.silent);
        const silent = rows.filter(r => r.silent);
        const items = [...alerting];
        if (silent.length > 0)
            items.push(this._header, ...silent);
        if (rows.length > 0)
            items.push(this._footer);
        this._items = items;

        this._sections = [alerting, silent];
        for (const section of this._sections) {
            section.forEach((row, i) => {
                row.sectionTop = i === 0;
                row.sectionBottom = i === section.length - 1;
                if (!row.dismissing)
                    row.setBaseRoundness(row.sectionTop ? 1 : 0, row.sectionBottom ? 1 : 0, animated);
            });
        }

        let y = 0;
        items.forEach((item, i) => {
            if (i > 0 && item === this._header)
                y += SECTION_GAP;
            let state = this._layout.get(item);
            if (!state || state.leaving) {
                // coming in: in place, fading in
                state = {offset: y, h: 0, alpha: animated ? 0 : 1, p: 0};
                this._layout.set(item, state);
                item.visible = true;
            }
            let h;
            if (item instanceof Row) {
                item.expanded = item.userExpanded ?? (i === 0 && !item.silent);
                item.setChevron(item.expanded);
                state.toP = item.expanded ? 1 : 0;
                if (state.h === 0) {
                    state.p = state.toP;
                    state.h = item.heightFor(state.p);
                }
                h = item.heightFor(state.toP);
            } else {
                h = item === this._header ? HEADER_H : FOOTER_H;
                if (state.h === 0)
                    state.h = h;
            }
            // Clear all hides the footer (FooterViewState.hideContent)
            const alpha = item === this._footer && this._clearing ? 0 : 1;
            Object.assign(state, {toOffset: y, toH: h, toAlpha: alpha});
            y += h + GAP;
        });
        // what left the stack fades where it is
        for (const [actor, state] of this._layout) {
            if (items.includes(actor))
                continue;
            state.leaving = true;
            Object.assign(state, {toOffset: state.offset, toH: state.h, toAlpha: 0, toP: state.p});
        }
        this._emptyAlpha.to = rows.length === 0 && !this._clearing ? 1 : 0;

        this._transition?.stop();
        this._transition = null;
        for (const state of this._layout.values())
            Object.assign(state, {fromOffset: state.offset, fromH: state.h, fromAlpha: state.alpha, fromP: state.p});
        this._emptyAlpha.from = this._emptyAlpha.now;
        if (!animated) {
            this._step(1);
            this._settle();
            return;
        }
        this._transition = animate(this, {
            from: 0, to: 1, duration: STANDARD_MS, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: e => this._step(e),
            onDone: () => {
                this._transition = null;
                this._settle();
            },
        });
    }

    _step(e) {
        for (const [actor, state] of this._layout) {
            state.offset = lerp(state.fromOffset, state.toOffset, e);
            state.h = lerp(state.fromH, state.toH, e);
            state.alpha = lerp(state.fromAlpha, state.toAlpha, e);
            state.p = lerp(state.fromP ?? 0, state.toP ?? 0, e);
            if (actor instanceof Row)
                actor.setExpansion(state.p);
        }
        this._emptyAlpha.now = lerp(this._emptyAlpha.from, this._emptyAlpha.to, e);
        this._place();
    }

    // drop what has faded out
    _settle() {
        for (const [actor, state] of [...this._layout]) {
            if (!state.leaving)
                continue;
            this._layout.delete(actor);
            if (actor instanceof Row)
                actor.destroy();
            else
                actor.visible = false;
        }
    }

    _place() {
        // what scrolls above the scrim's top is cut there
        this._scroll = clamp(this._scroll, 0, this._maxScroll);
        if (this._scroll > 0)
            this.set_clip(0, px(this._stackY), px(SCREEN_W), px(SCREEN_H - this._stackY));
        else
            this.remove_clip();
        const top = this._stackY + TOP_PADDING - this._scroll;
        for (const [actor, state] of this._layout) {
            const x = actor instanceof Row ? SIDE : 0;
            actor.set_position(px(x), px(top + this._ratio * state.offset));
            actor.setVisibleHeight(state.h * this._ratio);
            actor.opacity = Math.round(255 * state.alpha);
        }
        // "No notifications", centred in the space under the QQS (StackScrollAlgorithm: EmptyShadeView)
        const emptyAlpha = this._emptyAlpha.now;
        this._empty.visible = emptyAlpha > 0;
        if (emptyAlpha > 0) {
            const [w, h] = this._empty.get_size();
            this._empty.set_position(px(SCREEN_W / 2) - w / 2, px((this._stackY + SCREEN_H) / 2) - h / 2);
            this._empty.opacity = Math.round(255 * emptyAlpha);
        }
    }

    // --- swipes

    // the rows a swipe pulls along: the swiped one's section, in order (NotificationTargetsHelper)
    magneticRows(row) {
        const section = this._sections.find(rows => rows.includes(row)) ?? [row];
        return section.filter(r => r === row || !r.dismissing);
    }

    onSwipeBegin(_row) {
    }

    onSwipeEnd(_row) {
    }

    onDismissed(row) {
        if (this._clearing)
            return;
        row.notification?.destroy(MessageTray.NotificationDestroyedReason.DISMISSED);
    }

    // --- Clear all: the rows on screen slide right bottom-up, then the notifications go and the shade closes

    clearAll() {
        if (this._clearing)
            return;
        const rows = this._items.filter(item => item instanceof Row && !item.dismissing);
        if (rows.length === 0)
            return;
        this._clearing = true;
        const footer = this._layout.get(this._footer);
        if (footer)
            this._fadeFooter = animate(this, {
                from: footer.alpha, to: 0, duration: ESCAPE_MS, interpolator: FAST_OUT_SLOW_IN,
                onUpdate: v => {
                    footer.alpha = v;
                    this._footer.opacity = Math.round(255 * v);
                },
                onDone: () => {
                    this._fadeFooter = null;
                },
            });

        const onScreen = rows.filter(row => this._stackY + TOP_PADDING +
            this._ratio * (this._layout.get(row)?.offset ?? 0) < SCREEN_H);
        let delay = 0, step = CLEAR_DELAY, left = onScreen.length;
        const finish = () => {
            for (const row of rows)
                row.notification?.destroy(MessageTray.NotificationDestroyedReason.DISMISSED);
            this._shade.close();
        };
        for (const row of [...onScreen].reverse()) {
            row.setRoundness(1, 1, false);
            row.dismiss(0, true, {
                delay,
                onDone: () => {
                    if (--left === 0)
                        finish();
                },
            });
            delay += step;
            step = Math.max(step - CLEAR_DELAY_STEP, CLEAR_DELAY_MIN);
        }
        if (left === 0)
            finish();
    }

    // --- the shade opening and closing

    _shadeVisibleChanged() {
        if (this._shade.visible) {
            for (const row of this._rows.values())
                row.updateTime();
            this._relayout(false);
        } else {
            this._clearing = false;
            this._fadeFooter?.stop();
            this._fadeFooter = null;
            magnetic.reset();
            for (const row of this._rows.values())
                row.resetSwipe();
            this.stopScroll();
            this._scroll = 0;
            this._relayout(false);
        }
    }

    _onDestroy() {
        this._transition?.stop();
        this._scrollAnim?.stop();
        this._fadeFooter?.stop();
        if (this._updateId)
            GLib.source_remove(this._updateId);
        this._undoHints();
        for (const n of this._rows.keys())
            n.disconnectObject(this);
    }
});
