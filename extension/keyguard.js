// The keyguard: Android 16's lock screen over everything while the phone is locked, its bouncer, and the unlock
// (systemui.md §6, keyguard-pin.md, keyguard-extras.md; screen px of the 1080x2280 reference).
//
// Locked, the shell is in the 'unlock-dialog' session mode, as GNOME's own screen shield puts it: windows hidden,
// gestures, heads-ups and keybindings off, and this view holds a modal grab. It locks when the screen goes off
// (power.js) and when the session starts. de.yesman.neo lock-type picks the bouncer: the PIN pad, the pattern, or none
// (swipe). The secret is the user's password, checked by gp-auth through PAM; wrong attempts are throttled as the
// AOSP gatekeeper does, kept across restarts in ~/.local/state/gnome-phone/keyguard.json. That file also keeps the
// length of the last PIN that unlocked, for Android 14's auto-confirm: a PIN of 6 or more digits unlocks as its last
// digit is typed.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeBG from 'gi://GnomeBG?version=4.0';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

import {PatternView, PinView, Strings, touchSlot, tryAgainIn} from './bouncer.js';
import {fingerState, FingerStrings} from './finger.js';
import {FingerHint, FingerSensor, PinButton} from './fingerhint.js';
import {vibrate} from './haptics.js';
import {Canvas, paintIcon} from './icons.js';
import {fingerAllowed, mayOcclude, secretEntered as secretIsEntered, secretRequired as secretIsRequired,
    voiceUnlockAllowed} from './lockover.js';
import {animate, clamp, EMPHASIZED_ACCELERATE, FAST_OUT_SLOW_IN, lerp, LINEAR, MIN_FLING_VELOCITY,
    TOUCH_RESPONSE, VelocityTracker} from './motion.js';
import {Color, onRestyle, offRestyle, rgba, SHADE_PANEL} from './palette.js';
import {chargingText} from './state.js';
import {notificationIcon, SystemIcons} from './statusbar.js';
import {AndroidText} from './text.js';
import {wholeFrames} from './redraws.js';
import {px, screenPixels, SCALE, SCREEN_H, SCREEN_W} from './units.js';

Gio._promisify(Gio.Subprocess.prototype, 'communicate_utf8_async');
Gio._promisify(Gio.Subprocess.prototype, 'wait_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'replace_contents_async');

// the lock screen's checker, and the voice service's (pinask.js): the first one that is there
export const GP_AUTH = ['/usr/local/libexec/gp-auth', '/usr/libexec/gp-auth'];

// Voice unlock: the switch Settings owns (de.yesman.neo voice-unlock, off by default, with the warning that a recording
// can fool it), the Minerva voice service that answers the live check, and the words the lock screen adds around it.
const VOICE_KEY = 'voice-unlock';
const VOICE_NAME = 'org.minerva.Voice1';
const VOICE_PATH = '/org/minerva/Voice1';
const VOICE_IFACE = 'org.minerva.Voice1';
const VOICE_WORD = 'Sblocca con la voce';
const VoiceStrings = {
    listening: 'Ascolto, parla ora',
    miss: 'Voce non riconosciuta, riprova',
    tooMany: 'Troppi tentativi, usa il PIN',
    unavailable: 'Sblocco vocale non disponibile',
};

// A finger on the side sensor (gp-auth --fingerprint, pam_fprintd: one try per run) unlocks. The sensor is in the
// power key and listens while the phone is locked, screen off too: with the screen off a result counts only with the
// key's press that wakes the phone (Xiaomi's "press to unlock"), a touch in a pocket alone does nothing.
// The miss limit, the re-arm retries and their delay are de.yesman.neo's (finger-max-misses, finger-retry-count,
// finger-retry-delay-ms); after the misses only the PIN or pattern does, until it is used, and after a restart and a
// Lockdown too. The screen shows which of those it is, on the right edge at the power key (finger.js, fingerhint.js).
// a run that ends without a finger this soon found nothing to match with (no sensor, no prints, fprintd busy): the
// re-arm is retried
const FINGER_GAVE_UP_US = 3 * 1000 * 1000;
// Android's biometric reject: two short buzzes
const FINGER_MISS_BUZZ = [[1, 30], [0, 80], [1, 30]];
// a result with the screen off and the power key's wake press belong together this close (either comes first)
const FINGER_PRESS_WINDOW_US = 2 * 1000 * 1000;

// logind's LockedHint follows the keyguard, as Shell's own screen shield sets it: loginctl and Minerva's phone_state
// read it to know whether the phone is locked
function setLockedHint(locked) {
    Gio.DBus.system.call('org.freedesktop.login1', '/org/freedesktop/login1/session/auto',
        'org.freedesktop.login1.Session', 'SetLockedHint', new GLib.Variant('(b)', [locked]),
        null, Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
            try {
                bus.call_finish(res);
            } catch (e) {
                console.warn(`keyguard: SetLockedHint(${locked}): ${e.message}`);
            }
        });
}

// --- the lock screen (§6.1-6.3)
// The secret checked by gp-auth through PAM, as the bouncer's and the voice service's PIN sheet's (pinask.js): its
// exit status, 0 right, 1 wrong, 2 could not check (no helper, or PAM did not run)
export async function checkPin(secret) {
    const helper = GP_AUTH.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
    if (!helper) {
        log('gnome-phone: no gp-auth to check the lock screen secret with');
        return 2;
    }
    try {
        const proc = Gio.Subprocess.new([helper],
            Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
        await proc.communicate_utf8_async(`${secret}\n`, null);
        return proc.get_if_exited() ? proc.get_exit_status() : 2;
    } catch (e) {
        logError(e, 'gnome-phone: gp-auth');
        return 2;
    }
}

const CLOCK_SIZE = 86 * 0.98;           // dp, the small Flex clock
const CLOCK_WGHT = 550;                 // matched to the reference's ink: 582 px for "05:12"
const CLOCK_LEFT = 84;                  // 28 dp
const CLOCK_INK_TOP = 246;
const DATE_SIZE = 16;
const DATE_WGHT = 630;                  // ink 262 px for "Wed, Nov 4"
const DATE_INK_TOP = 541.5;
const CARRIER_LEFT = 48;
const CARRIER_INK_TOP = 42;
const SYSTEM_END = 880;                 // 9 px left of the home status bar's (keyguard-extras §C.3)
const PILL = {x: 48, y: 664, h: 144, pad: 39, slot: 66, icon: 45, max: 3};
const LOCK_ICON = {cx: 540, cy: 1950, size: 65 * 2.54};
const INDICATION_SIZE = 14;
const INDICATION_WGHT = 700;            // body 400 + Bold text's 300: stems 6-7 px, cap height 32 at opsz 16
const INDICATION_INK_TOP = 2083;

// --- the swipe up (§6.4): f is the panel's expansion, 1 at rest; f = 1 - (d + 142) / 1590 for a drag of d px
const SLOP = 24;
const F_OFFSET = 142;
const F_RANGE = 1590;
const BOUNCER_SHIFT = 240;              // keyguard_host_view_translation_y 80 dp
const BLUR_MAX = 102;                   // 34 dp
const SETTLE_MS = 300;
const CANCEL_MS = 100;
const DISMISS_SWIPE = 30;               // a bouncer dragged down 10 dp goes on release
const BACK_EDGE = 90;                   // config_backGestureInset 30 dp: the bouncer leaves these touches to back
const BOUNCER_TO_LOCK_MS = 450;
const LOCK_ALPHA_BACK_MS = 250;

// --- the unlock (keyguard-extras §D)
const GONE_SHORT_MS = 200;
const GONE_MS = 500;
const SURFACE_DELAY_MS = 67;
const SURFACE_MS = 300;
const SURFACE_SCALE = 0.95;
const SURFACE_SHIFT = Math.round(SCREEN_H * 0.05);   // 5 % of the screen's height: 114 on 2280
const SURFACE_PIVOT_Y = 0.66;
// swipe (no secret): dismissing past 0.3 unlocks; the content fades out by 0.18 and rises up to 180 dp
const SWIPE_FINISH = 0.3;
const SWIPE_RISE = 540;

// auto-confirm: AOSP's MIN_AUTO_PIN_REQUIREMENT_LENGTH
const MIN_AUTO_CONFIRM = 6;
// frames the bouncer is painted at opacity 1/255 when it is built, so its canvases are drawn and uploaded then
const PRERENDER_FRAMES = 3;

const showBouncerProgress = f => f >= 0.9 ? 1 : f < 0.6 ? 0 : (f - 0.6) / 0.3;
// LockscreenToPrimaryBouncer: progress over f 1 -> 0.88, the lock screen gone by half of it
const toBouncerProgress = f => 1 - clamp((f - 0.88) / 0.12);

// the AOSP gatekeeper's retry timeout after n failures, s
function lockoutSeconds(n) {
    if (n < 5)
        return 0;
    if (n < 11)
        return n === 5 || n === 10 ? 30 : 0;
    if (n < 30)
        return 30;
    if (n < 140)
        return 30 * 2 ** Math.floor((n - 30) / 10);
    return 86400;
}

function neoSettings() {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup('de.yesman.neo', true);
    return schema ? new Gio.Settings({settings_schema: schema}) : null;
}

// an int of de.yesman.neo, 0 when the settings or the key are not there: a schema compiled before the key existed
// would abort the shell on get_int
function neoInt(settings, key) {
    return settings?.settings_schema?.has_key(key) ? settings.get_int(key) : 0;
}

// One line of AndroidText placed by its ink: left at its logical start (or centred), its ink top at a y
function placeText(text, {left, centre, inkTop}) {
    const x = centre !== undefined ? centre - text.textWidth / 2 : left;
    text.set_position(px(x - text.inset), px(inkTop - text.inset - text.ink.y));
}

const LockIcon = GObject.registerClass(
class LockIcon extends Canvas {
    constructor() {
        super({width: px(LOCK_ICON.size), height: px(LOCK_ICON.size), reactive: true});
        this.set_position(px(LOCK_ICON.cx - LOCK_ICON.size / 2), px(LOCK_ICON.cy - LOCK_ICON.size / 2));
        this._name = 'keyguard_ic_lock';
    }

    set unlocked(on) {
        const name = on ? 'keyguard_ic_unlocked' : 'keyguard_ic_lock';
        if (name !== this._name) {
            this._name = name;
            this.queue_repaint();
        }
    }

    draw(cr) {
        screenPixels(cr);
        paintIcon(cr, this._name, 0, 0, LOCK_ICON.size, Color.accent1_100);
    }
});


// GNOME's wallpaper as a picture St can draw. A slideshow (an .xml: Debian's default, desktop-base's
// desktop-background.xml, is one) is its picture for now at the panel's size, as the shell's own background takes it;
// St would try to load the XML as an image on every frame.
function pictureUri(uri) {
    if (!uri?.toLowerCase().endsWith('.xml'))
        return uri;
    try {
        const show = new GnomeBG.BGSlideShow({file: Gio.File.new_for_uri(uri)});
        show.load();
        const [, , , file] = show.get_current_slide(SCREEN_W, SCREEN_H);
        return file ? Gio.File.new_for_path(file).get_uri() : '';
    } catch (e) {
        logError(e, `gnome-phone: the wallpaper slideshow ${uri}`);
        return '';
    }
}

export class Keyguard extends Signals.EventEmitter {
    // @state: the SystemState (carrier, battery, the status icons)
    constructor(state) {
        super();
        this._state = state;
        this._neo = neoSettings();
        // the wallpaper is the launcher's (GNOME's background, its dark picture under the dark style), as Shell 51's
        // own unlock dialog has it: Settings' Background panel no longer sets the screensaver's picture
        this._background = new Gio.Settings({schema_id: 'org.gnome.desktop.background'});
        this._interface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        // Settings' "Show on the lock screen" (Notifications), for the pill's icons
        this._notificationSettings = new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'});
        this.showing = false;
        this.bouncerShowing = false;
        this._bouncerActive = false;
        this._pinLength = 0;
        this._f = 1;
        this._bouncer = null;
        this._modeLocked = false;
        this._grab = null;
        this._afterRestart = true;
        this._failures = 0;
        this._lockedUntil = 0;          // realtime, ms
        this._countdownId = 0;
        this._checking = false;
        this._finger = null;            // the running gp-auth --fingerprint
        this._voice = null;             // the running gp-auth --voice
        this._voiceLine = null;         // its one line of words
        this._fingerMisses = 0;
        this._fingerPending = null;     // a screen-off result waiting for the power key's press (wokeUp)
        this._fingerRetries = 0;        // re-arms spent since the sensor last held the device
        this._fingerRetryId = 0;
        this._fingerSignal = '';        // the last fprintd VerifyStatus state, while it still says something
        this._fingerHint = null;
        this._fingerSensor = null;
        this._wokeByPressAt = 0;
        this._awake = true;             // the screen is on (power.js: goToSleep / wakeUp)
        // an app's window over the lock screen (setOccluded), and the other windows' actors hidden meanwhile
        this.occludedBy = null;
        // the lock screen has the front back, for the unlock under way: no window goes over it meanwhile
        this._revealing = false;
        this._occlusionHidden = new Set();
        this._occlusion = {};           // the connections that last while occluded
        this._loadThrottle();
        // the last answer of secretRequired, kept to tell whenlocked.js to look again (emits 'changed')
        this._secretRequired = this.secretRequired;
        this._secretEntered = this.secretEntered;

        this._build();
        this._fingerSensor = new FingerSensor(state => {
            // only a run the lock screen started says what this phone's sensor is doing
            if (!this._finger)
                return;
            this._fingerSignal = state;
            this._syncFingerHint();
        });
        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._syncClock(), this);
        this._interface.connectObject('changed::clock-format', () => this._syncClock(),
            'changed::color-scheme', () => this._syncWallpaper(), this);
        this._background.connectObject('changed', () => this._syncWallpaper(), this);
        this._notificationSettings.connectObject('changed::show-in-lock-screen', () => this._syncPill(), this);
        state.connectObject('changed', () => this._syncState(), this);
        // another lock type: the next lock builds its bouncer
        this._neo?.connectObject('changed::lock-type', () => {
            this._syncFingerHint();
            this._applyOcclusion();
            this._syncSecretRequired();
            if (this._bouncerActive)
                return;
            this._endPrerender();
            this._bouncer?.destroy();
            this._bouncer = null;
        }, this);
        Main.messageTray.connectObject(
            'source-added', (_tray, source) => this._addSource(source),
            'source-removed', (_tray, source) => {
                source.disconnectObject(this);
                this._syncPill();
            }, this);
        for (const source of Main.messageTray.getSources())
            this._addSource(source);
        this._syncClock();
        this._syncState();
        this._syncWallpaper();
    }

    get _lockType() {
        return this._neo?.get_string('lock-type') ?? 'pin';
    }

    // the swipe up's slop, in dp, before it starts counting as one (de.yesman.neo lock-swipe-slop)
    get _swipeSlop() {
        const dp = neoInt(this._neo, 'lock-swipe-slop');
        return dp > 0 ? dp * SCALE : SLOP;
    }

    get _secure() {
        return this._lockType !== 'swipe';
    }

    // The phone wants its secret before anything else: the first unlock after a boot or a shell restart, a Lockdown,
    // the PIN's own timeout, the sensor out of misses (lockover.js). No window shows over the lock screen meanwhile
    // (whenlocked.js asks this), so the way to the PIN pad stays on the screen.
    get secretRequired() {
        return secretIsRequired({
            secure: this._secure,
            afterRestart: this._afterRestart,
            lockedDown: this._lockedDown,
            lockedOut: this._lockedOut,
            misses: this._fingerMisses,
            max: this._fingerMaxMisses,
        });
    }

    // The secret was entered since the boot or the shell started (lockover.js): what org.minerva.ScreenWake1 tells the
    // Minerva services, which engage with nothing before it. Lockdown and the PIN timeout do not take it back.
    get secretEntered() {
        return secretIsEntered({secure: this._secure, afterRestart: this._afterRestart});
    }

    // the sensor's policy, from de.yesman.neo: no settings, no sensor (the schema is installed with the extension,
    // and a schema too old to know a key reads as 0: no arming, never a crash)
    get _fingerMaxMisses() {
        return neoInt(this._neo, 'finger-max-misses');
    }

    get _fingerRetryCount() {
        return neoInt(this._neo, 'finger-retry-count');
    }

    get _fingerRetryDelayMs() {
        return neoInt(this._neo, 'finger-retry-delay-ms');
    }

    // --- the view

    _build() {
        const monitor = Main.layoutManager.primaryMonitor;
        this.actor = new St.Widget({
            name: 'gnomePhoneKeyguard',
            reactive: true,
            visible: false,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
            style: 'background-color: black;',
        });
        // Plain actors first and last: what shows and hides between them (the bouncer, the wallpaper) never changes
        // the first or last visible child. St restyles a child that gains or loses :first-child or :last-child, and a
        // restyle redraws every canvas under it: the bouncer showing restyled all its keys, mid-swipe.
        this.actor.add_child(new Clutter.Actor());
        this._wallpaper = new St.Widget({width: monitor.width, height: monitor.height});
        this.actor.add_child(this._wallpaper);

        // the bouncer's backdrop: the wallpaper blurred, under the shade panel's tint; in the unlock, the screen coming
        // up behind. Drawn in whole frames while it shows (redraws.js).
        this._blurLayer = new St.Widget({width: monitor.width, height: monitor.height});
        this._blur = new Shell.BlurEffect({mode: Shell.BlurMode.BACKGROUND, radius: 0, brightness: 1});
        this._blur.enabled = false;
        this._blurLayer.add_effect(this._blur);
        this.actor.add_child(this._blurLayer);
        this._scrim = new St.Widget({width: monitor.width, height: monitor.height, opacity: 0,
            style: this._scrimStyle()});
        this.actor.add_child(this._scrim);

        // what the lock screen shows
        this._content = new St.Widget({width: monitor.width, height: monitor.height});
        this.actor.add_child(this._content);
        this._carrier = new AndroidText('', 14, 700, {colour: Color.onWall, opsz: 16});
        this._content.add_child(this._carrier);
        this._systemIcons = new SystemIcons(this._state, {end: SYSTEM_END});
        this._content.add_child(this._systemIcons);
        this._clock = new AndroidText('', CLOCK_SIZE, CLOCK_WGHT, {colour: Color.clock});
        this._content.add_child(this._clock);
        this._date = new AndroidText('', DATE_SIZE, DATE_WGHT, {colour: Color.onWall});
        this._content.add_child(this._date);
        this._pill = new St.Widget({x: px(PILL.x), y: px(PILL.y), height: px(PILL.h), visible: false,
            style: this._pillStyle()});
        this._content.add_child(this._pill);
        this._lockIcon = new LockIcon();
        this._content.add_child(this._lockIcon);
        this._lockIcon.connect('button-release-event', () => this._lockIconTapped());
        this._lockIcon.connect('touch-event', (a, event) => {
            if (event.type() === Clutter.EventType.TOUCH_END && !this._tracking)
                this._lockIconTapped();
            return Clutter.EVENT_PROPAGATE;
        });
        this._indication = new AndroidText('', INDICATION_SIZE, INDICATION_WGHT, {colour: Color.onWall, opsz: 16});
        this._content.add_child(this._indication);
        // The way to the PIN pad while an app's window shows over the lock screen (setOccluded): the swipe up is not
        // there to be found, this is (fingerhint.js). The lock screen's own child, so it takes the touch its modal
        // grab holds; hidden while the lock screen is itself, where the lock icon and the swipe up do.
        this._pinButton = new PinButton(() => this._pinTapped());
        this.actor.add_child(this._pinButton);
        // voice unlock (docs/voice-unlock.md): the owner's voice, only when Settings offers it and the rules allow it
        this._voiceButton = new PinButton(() => this._voiceTapped());
        this._voiceButton.setText(VOICE_WORD, Math.round(SCREEN_H / 2) + 268);
        this.actor.add_child(this._voiceButton);
        // the sensor's state, on the right edge at the power key (fingerhint.js). In a layer of its own, over the lock
        // screen and over a window that shows over it (setOccluded): there this view draws nothing, and the state of
        // the sensor is all that stays on the screen.
        this._fingerHint = new FingerHint();
        this.hintLayer = new St.Widget({x: monitor.x, y: monitor.y, width: monitor.width, height: monitor.height,
            reactive: false, visible: false});
        this.hintLayer.add_child(this._fingerHint);

        this._bouncerBox = new St.Widget({width: monitor.width, height: monitor.height, opacity: 0, visible: false});
        this.actor.add_child(this._bouncerBox);
        // the bouncer carries its own messages: the hint goes while it is up, and comes back with the lock screen
        this._bouncerBox.connect('notify::visible', () => this._syncFingerHint());
        this.actor.add_child(new Clutter.Actor());

        // the look is live: what was copied into styles follows (the text and the lock icon are Canvases)
        onRestyle(this, () => {
            this._scrim.style = this._scrimStyle();
            this._pill.style = this._pillStyle();
            this._syncPill();
            this._fingerHint?.restyle();
            this._pinButton?.restyle();
        });

        this._tracker = new VelocityTracker();
        this._down = null;
        this.actor.connect('event', (a, event) => this._onEvent(event));
    }

    _scrimStyle() {
        return `background-color: ${rgba(SHADE_PANEL[0].map(Math.round), SHADE_PANEL[1])};`;
    }

    _pillStyle() {
        return `background-color: rgb(${Color.lockPill.map(Math.round).join(', ')}); border-radius: ${px(PILL.h / 2)}px;`;
    }

    _syncWallpaper() {
        const dark = this._interface.get_string('color-scheme') === 'prefer-dark';
        const uri = pictureUri((dark && this._background.get_string('picture-uri-dark')) ||
            this._background.get_string('picture-uri'));
        const style = uri ? `background-image: url("${uri}"); background-size: cover;`
            : 'background-color: black;';
        this._wallpaper.style = style;
        this._wallpaperStyle = style;
    }

    _syncClock() {
        const now = GLib.DateTime.new_now_local();
        const h24 = this._interface.get_string('clock-format') === '24h';
        this._clock.setText(now.format(h24 ? '%H:%M' : '%-I:%M'));
        placeText(this._clock, {left: CLOCK_LEFT, inkTop: CLOCK_INK_TOP});
        this._date.setText(now.format('%a, %b %-e'));
        placeText(this._date, {left: CLOCK_LEFT, inkTop: DATE_INK_TOP});
    }

    _syncState() {
        const {mobile, wifi, battery} = this._state;
        // the mobile network, then (the owner's addition to Android's carrier text) the Wi-Fi network, joined as
        // CarrierTextManager joins two SIMs' names
        const names = [mobile.present ? mobile.operator || 'Emergency calls only' : 'No SIM'];
        if (wifi.connected && wifi.name)
            names.push(wifi.name);
        const carrier = names.join(' | ');
        if (carrier !== this._carrierText) {
            this._carrierText = carrier;
            this._carrier.setText(carrier);
            placeText(this._carrier, {left: CARRIER_LEFT, inkTop: CARRIER_INK_TOP});
        }
        // KeyguardIndicationController's power line, with how fast (SystemState's speed; One UI's names)
        let indication = '';
        if (battery.present && battery.plugged)
            indication = battery.level >= 100 ? 'Charged'
                : `${battery.level}% • ${chargingText(battery.speed, battery.amps)}`;
        if (indication !== this._indicationText) {
            this._indicationText = indication;
            this._indication.setText(indication);
            this._indication.visible = !!indication;
            placeText(this._indication, {centre: 540, inkTop: INDICATION_INK_TOP});
        }
    }

    _addSource(source) {
        source.connectObject(
            'notification-added', () => this._syncPill(),
            'notification-removed', () => this._syncPill(), this);
        this._syncPill();
    }

    // the notification icons in their pill (keyguard-extras §B): one per app, three and the overflow dot at most;
    // none after a lockdown. Only the apps allowed on the lock screen: a source's policy says it for the setting and
    // the app's own switch together (show-in-lock-screen, both), as for Shell's own lock screen (unlockDialog.js).
    // An app's switch is read again at each lock (lock() syncs the pill), the setting at once.
    _syncPill() {
        this._pill.destroy_all_children();
        const sources = this._lockedDown ? []
            : Main.messageTray.getSources().filter(s => s.notifications.length && s.policy.showInLockScreen);
        const n = Math.min(sources.length, PILL.max + 1);
        this._pill.visible = n > 0;
        if (!n)
            return;
        this._pill.width = px((26 + 22 * n) * 3);
        sources.slice(0, sources.length > PILL.max + 1 ? PILL.max : n).forEach((source, k) => {
            const icon = notificationIcon(source, px(PILL.icon));
            icon.opacity = 255;
            icon.style = `color: rgb(${Color.onSurface.join(', ')});`;
            icon.set_position(px(PILL.pad + PILL.slot * k + (PILL.slot - PILL.icon) / 2), px((PILL.h - PILL.icon) / 2));
            this._pill.add_child(icon);
        });
        if (sources.length > PILL.max + 1) {
            const dot = new St.Widget({width: px(12), height: px(12),
                style: `background-color: rgba(255, 255, 255, ${0xdd / 255}); border-radius: ${px(6)}px;`});
            dot.set_position(px(PILL.pad + PILL.slot * PILL.max + PILL.slot / 2 - 6), px(PILL.h / 2 - 6));
            this._pill.add_child(dot);
        }
    }

    // --- locking

    lock() {
        if (this.showing) {
            // the screen went off over the bouncer: back to the lock screen
            this._hideBouncer(false);
            this._syncFinger();
            return;
        }
        this.showing = true;
        setLockedHint(true);
        this._fingerSignal = '';
        this._unlocking?.stop();
        this._lockIcon.unlocked = !this._secure;
        this._syncClock();
        this._syncState();
        this._syncPill();
        this._setF(1);
        this._content.opacity = 255;
        this._content.translation_y = 0;
        this._wallpaper.show();
        this.actor.show();
        if (!this._modeLocked) {
            Main.sessionMode.pushMode('unlock-dialog');
            this._modeLocked = true;
        }
        this._grab ??= Main.pushModal(this.actor, {actionMode: Shell.ActionMode.LOCK_SCREEN});
        this._prepareBouncer();
        this._syncFinger();
        this.emit('changed');
    }

    // the power menu's Lockdown: locked at once, and the notifications kept off the lock screen until the next unlock
    lockdown() {
        this._lockedDown = true;
        this.lock();
        this._syncPill();
        this._syncFinger();
    }

    // --- occluded: an app's window over the lock screen (whenlocked.js; KeyguardViewMediator.setOccluded)

    get revealing() {
        return this._revealing;
    }

    // @win: the window that shows over the lock screen, or null for the lock screen itself. This view keeps its place
    // and its modal grab: touches and keys reach the lock screen meanwhile, so the swipe up brings the bouncer and a
    // finger unlocks as on the lock screen itself, and nothing typed reaches the window (a kiosk disables its chat
    // meanwhile, chat.show_when_locked). Only this view's drawing steps aside: every other window stays hidden, and
    // the phone stays locked (showing stays true), so home, recents and the shade stay shut.
    setOccluded(win) {
        win = mayOcclude({showing: this.showing, revealing: this._revealing, requiresSecret: this.secretRequired,
            overWindow: win}) ? win : null;
        if (win === this.occludedBy)
            return;
        this._showHiddenWindows();
        this.occludedBy = win;
        if (win) {
            if (this._bouncerActive)
                this._hideBouncer(false);
            this._setF(1);
            // the group is hidden by the locked session mode: shown for this window, with every other window in it
            // hidden, the ones that come later too
            const group = global.window_group;
            group.connectObject(
                'notify::visible', () => group.show(),
                'child-added', (_group, actor) => this._hideForOcclusion(actor), this._occlusion);
            group.show();
            for (const actor of global.get_window_actors())
                this._hideForOcclusion(actor);
        } else if (this.showing) {
            Main.layoutManager._updateVisibility();
        }
        this._applyOcclusion();
        this._syncFingerHint();
        this.emit('changed');
    }

    // occluded: the screen's input target with nothing drawn, the window showing through it. The modal grab is not
    // touched here (it is the lock screen's, pushed by lock()): the window gets no touch and no key meanwhile.
    _applyOcclusion() {
        const behind = !!this.occludedBy;
        this.actor.style = behind ? '' : 'background-color: black;';
        this._wallpaper.visible = !behind;
        this._blurLayer.visible = !behind;
        this._scrim.visible = !behind;
        this._content.visible = !behind;
        // the way to the secret, where the lock screen's own icons are not: only a lock with a secret has one to give
        this._pinButton.visible = behind && this._secure;
    }

    // The lock screen takes the front back from the window over it, for the unlock that follows (the swipe's slop, a
    // finger). whenlocked.js keeps the window behind while this is true, and it comes back at rest (_atRest).
    _revealStart() {
        if (!this.occludedBy)
            return;
        this._revealing = true;
        this.occludedBy = null;
        this._showHiddenWindows();
        Main.layoutManager._updateVisibility();
        this._applyOcclusion();
        this._syncFingerHint();
        this.emit('changed');
    }

    // the lock screen at rest again (f 1, no bouncer): the window over it may come back on top
    _atRest() {
        if (!this._revealing || this._bouncerActive || this.bouncerShowing || this._f < 1)
            return;
        this._revealing = false;
        this.emit('changed');
    }

    _hideForOcclusion(actor) {
        if (actor === this.occludedBy?.get_compositor_private() || !actor.visible)
            return;
        actor.hide();
        this._occlusionHidden.add(actor);
        actor.connectObject('destroy', () => this._occlusionHidden.delete(actor), this._occlusion);
    }

    _showHiddenWindows() {
        global.window_group.disconnectObject(this._occlusion);
        for (const actor of this._occlusionHidden) {
            actor.disconnectObject(this._occlusion);
            actor.show();
        }
        this._occlusionHidden.clear();
    }

    // the screen came on: the lock screen as it was, the clock up to date
    // @reason: power.js's ('power' for the key, 'double-tap', 'screensaver'...)
    wokeUp(reason) {
        this._awake = true;
        this._wokeByPressAt = reason === 'power' ? GLib.get_monotonic_time() : 0;
        const pending = this._fingerPending;
        this._fingerPending = null;
        if (!this.showing)
            return;
        this._syncClock();
        this._syncState();
        // the finger landed on the key a moment before the press went through
        if (pending && this._wokeByPressAt && this._wokeByPressAt - pending.at < FINGER_PRESS_WINDOW_US)
            this._fingerDone(pending.status, pending.usec);
        else
            this._syncFinger();
    }

    // the screen went off (after lock()): the sensor keeps listening, for the press that wakes the phone
    asleep() {
        this._awake = false;
        this._wokeByPressAt = 0;
        this._fingerPending = null;
        this._syncFinger();
    }

    // --- a finger on the sensor

    get _fingerAllowed() {
        return fingerAllowed({showing: this.showing, secure: this._secure, afterRestart: this._afterRestart,
            lockedDown: this._lockedDown, unlocking: this._unlockingNow, lockedOut: this._lockedOut,
            misses: this._fingerMisses, max: this._fingerMaxMisses});
    }

    // The hint is redrawn on the same call sites as the arming: every flag above is one of its states.
    _syncFinger() {
        if (this._fingerAllowed)
            this._startFinger();
        else
            this._stopFinger();
        this._syncFingerHint();
        this._syncSecretRequired();
    }

    // whenlocked.js looks again when what the lock screen wants changes: the window over it steps aside for the PIN
    // and comes back once it has been given
    _syncSecretRequired() {
        this._syncSecretEntered();
        const required = this.secretRequired;
        if (required === this._secretRequired)
            return;
        this._secretRequired = required;
        this.emit('changed');
    }

    // 'secret-entered' tells voicewake.js to say so on the bus, the moment the first secret after a boot is given
    _syncSecretEntered() {
        const entered = this.secretEntered;
        if (entered === this._secretEntered)
            return;
        this._secretEntered = entered;
        this.emit('secret-entered', entered);
    }

    // the sensor's state on the right edge, at the power key (finger.js says what each one means)
    // voice unlock (docs/voice-unlock.md): the switch is Settings'; a schema compiled before the key existed reads as off
    _voiceUnlockOn() {
        try {
            const settings = neoSettings();
            return !!settings?.settings_schema.has_key(VOICE_KEY) && settings.get_boolean(VOICE_KEY);
        } catch (e) {
            return false;
        }
    }

    // a tap on "Sblocca con la voce": gp-auth --voice (PAM gnome-phone-voice: 0 the voice matched, 1 it did not, 3 nothing
    // to ask, 2 PAM could not run). A match unlocks by the path a finger takes; a miss shares the sensor's budget.
    async _voiceTapped() {
        if (this._voice || !this._voiceUnlockOn())
            return;
        const helper = GP_AUTH.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
        if (!helper)
            return;
        let proc;
        try {
            proc = Gio.Subprocess.new([helper, '--voice'], Gio.SubprocessFlags.NONE);
        } catch (e) {
            logError(e, 'gnome-phone: gp-auth --voice');
            return;
        }
        this._voice = proc;
        try {
            await proc.wait_async(null);
        } catch (e) {
        }
        this._voice = null;
        const status = proc.get_if_exited() ? proc.get_exit_status() : 2;
        if (status === 0) {
            this._fingerMisses = 0;
            this._noteUnlock('voice');
            if (this.occludedBy)
                this._unlockOver();
            else if (this.bouncerShowing && this._bouncer)
                this._bouncer.disappear(() => this._goingAway());
            else
                this._goingAway();
            return;
        }
        if (status === 1)
            this._fingerMisses++;
        this._syncFingerHint();
    }

    // tells the voice service a PIN or a finger just unlocked, so its 4 h window can open. Never blocks, never throws:
    // a missing service must not keep the phone locked.
    _noteUnlock(how) {
        try {
            Gio.DBus.session.call(VOICE_NAME, VOICE_PATH, VOICE_IFACE, 'NoteUnlock', new GLib.Variant('(s)', [how]),
                null, Gio.DBusCallFlags.NONE, 2000, null, (bus, res) => {
                    try {
                        bus.call_finish(res);
                    } catch (e) {
                        // the voice service is not running or does not know the call
                    }
                });
        } catch (e) {
            // no session bus: nothing to tell
        }
    }

    _syncFingerHint() {
        if (!this._fingerHint)
            return;
        // unlocked there is nothing to say: finger.js's states do not know about the lock screen going away
        if (!this.showing) {
            this._fingerHint.visible = false;
            this.hintLayer.visible = false;
            return;
        }
        const kind = this._lockType === 'pattern' ? 'pattern' : 'pin';
        this._pinButton?.setKind(kind);
        if (this._voiceButton)
            this._voiceButton.visible = voiceUnlockAllowed({
                enabled: this._voiceUnlockOn(), showing: this.showing, secure: this._secure,
                afterRestart: this._afterRestart, lockedDown: this._lockedDown,
                unlocking: this._unlockingNow, lockedOut: this._lockedOut,
                misses: this._fingerMisses, max: this._fingerMaxMisses}) && !(this._bouncerBox?.visible ?? false);
        this._fingerHint.setState(fingerState({
            secure: this._secure,
            bouncer: this._bouncerBox?.visible ?? false,
            lockedDown: this._lockedDown,
            afterRestart: this._afterRestart,
            lockedOut: this._lockedOut,
            misses: this._fingerMisses,
            max: this._fingerMaxMisses,
            signal: this._fingerSignal,
            armed: !!this._finger,
        }), kind);
        this.hintLayer.visible = this._fingerHint.visible;
    }

    // an arming that found no sensor is retried, as de.yesman.neo allows: fprintd off the bus, the device not
    // claimed, the helper missing. Retries are not attempts: only a run's own miss counts against the limit.
    _scheduleFingerRetry() {
        if (this._fingerRetryId || !this._fingerAllowed || this._fingerRetries >= this._fingerRetryCount)
            return;
        this._fingerRetries++;
        this._fingerRetryId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._fingerRetryDelayMs, () => {
            this._fingerRetryId = 0;
            // through the guard, which also redraws the hint
            this._syncFinger();
            return GLib.SOURCE_REMOVE;
        });
    }

    _cancelFingerRetry() {
        if (this._fingerRetryId)
            GLib.source_remove(this._fingerRetryId);
        this._fingerRetryId = 0;
    }

    async _startFinger() {
        if (this._finger)
            return;
        const helper = GP_AUTH.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
        if (!helper) {
            this._scheduleFingerRetry();
            return;
        }
        let proc;
        try {
            proc = Gio.Subprocess.new([helper, '--fingerprint'],
                Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            logError(e, 'gnome-phone: gp-auth --fingerprint');
            this._scheduleFingerRetry();
            return;
        }
        this._finger = proc;
        this._fingerSensor?.start();
        const started = GLib.get_monotonic_time();
        try {
            await proc.wait_async(null);
        } catch (e) {
        }
        // stopped meanwhile (_stopFinger): its answer no longer counts
        if (this._finger !== proc)
            return;
        this._finger = null;
        const status = proc.get_if_exited() ? proc.get_exit_status() : 2;
        this._fingerDone(status, GLib.get_monotonic_time() - started);
    }

    // SIGINT: pam_fprintd stops the verify and lets the sensor go
    _stopFinger() {
        this._cancelFingerRetry();
        this._fingerRetries = 0;
        this._fingerSignal = '';
        const proc = this._finger;
        this._finger = null;
        proc?.send_signal(2);
    }

    // @status: gp-auth --fingerprint's, 0 a match, 1 a miss, 3 no finger (timeout) or nothing to match, 2 PAM failed
    _fingerDone(status, usec) {
        if (!this._fingerAllowed) {
            this._syncFingerHint();
            return;
        }
        // screen off: the result waits for the power key's press (wokeUp), a touch alone is neither a match nor a miss
        if (!this._awake && (status === 0 || status === 1)) {
            this._fingerPending = {status, usec, at: GLib.get_monotonic_time()};
            this._syncFinger();
            return;
        }
        if (status === 0) {
            this._fingerMisses = 0;
            this._fingerRetries = 0;
            this._fingerSignal = '';
            this._noteUnlock('fingerprint');
            // a window over the lock screen: the phone unlocks behind it, where the user is
            if (this.occludedBy)
                this._unlockOver();
            else if (this.bouncerShowing && this._bouncer)
                this._bouncer.disappear(() => this._goingAway());
            else
                this._goingAway();
            return;
        }
        if (status === 1) {
            this._fingerMisses++;
            this._fingerSignal = 'miss';
            vibrate(FINGER_MISS_BUZZ);
            if (this._bouncerActive && !this._lockedOut && !this._checking) {
                this._bouncer.setMessage(this._fingerMisses < this._fingerMaxMisses ? FingerStrings.miss
                    : FingerStrings.tooMany[this._lockType === 'pattern' ? 'pattern' : 'pin'],
                this._fingerMisses < this._fingerMaxMisses ? '' : this._strings.prompt);
            }
            // a rejected touch: armed again at once, at the miss limit or not (the guard says whether it may be)
            this._syncFinger();
            return;
        }
        // no finger at all, or PAM could not run. A run that waited out its window held the sensor: it is re-armed
        // now, and the retry budget starts again. One that ended at once never found the sensor: the budget pays.
        if (status === 3 && usec >= FINGER_GAVE_UP_US) {
            this._fingerRetries = 0;
            this._fingerSignal = '';
            this._syncFinger();
            return;
        }
        this._fingerSignal = '';
        this._scheduleFingerRetry();
        this._syncFingerHint();
    }

    // --- the swipe up and the bouncer

    _local(event) {
        const [sx, sy] = event.get_coords();
        return [sx * SCALE, sy * SCALE];
    }

    _onEvent(event) {
        if (!this.showing || this._unlockingNow)
            return Clutter.EVENT_PROPAGATE;
        const type = event.type();
        const seq = touchSlot(event);
        const isTouch = type >= Clutter.EventType.TOUCH_BEGIN && type <= Clutter.EventType.TOUCH_CANCEL;
        if (type === Clutter.EventType.TOUCH_BEGIN || type === Clutter.EventType.BUTTON_PRESS) {
            if (this._down)
                return Clutter.EVENT_STOP;
            const [x, y] = this._local(event);
            // a touch stopped here cancels the back gesture before it can recognize
            if (this.bouncerShowing && isTouch && (x < BACK_EDGE || x > SCREEN_W - BACK_EDGE))
                return Clutter.EVENT_PROPAGATE;
            this._settle?.stop();
            this._settle = null;
            // a touch while the bouncer is still coming up: it is up, and the touch is the bouncer's
            if (this._bouncerActive && !this.bouncerShowing) {
                this._setF(0);
                this._bouncerShown();
            }
            this._down = {seq: isTouch ? seq : 'pointer', x, y, t: event.get_time(), f: this._f};
            this._tracking = false;
            this._tracker.clear();
            this._tracker.add(event.get_time(), x, y);
            return Clutter.EVENT_STOP;
        }
        const down = this._down;
        const mine = down && (isTouch ? down.seq !== 'pointer' && down.seq === seq
            : down.seq === 'pointer' && (type === Clutter.EventType.MOTION || type === Clutter.EventType.BUTTON_RELEASE));
        if (!mine)
            return Clutter.EVENT_PROPAGATE;
        const [x, y] = this._local(event);
        this._tracker.add(event.get_time(), x, y);
        if (type === Clutter.EventType.TOUCH_UPDATE || type === Clutter.EventType.MOTION) {
            this._drag(down, x, y);
            return Clutter.EVENT_STOP;
        }
        this._down = null;
        this._release(down, x, y, type !== Clutter.EventType.TOUCH_CANCEL);
        return Clutter.EVENT_STOP;
    }

    _drag(down, x, y) {
        const d = down.y - y;
        if (this.bouncerShowing) {
            // a bouncer dragged down goes back to the lock screen on release
            this._bouncerBox.translation_y = px(Math.max(0, -d) * 0.5);
            return;
        }
        if (!this._tracking) {
            if (d < this._swipeSlop)
                return;
            this._tracking = true;
            // over a window (setOccluded): the lock screen comes back for the rest of this unlock
            this._revealStart();
            if (this._secure)
                this._ensureBouncer();
        }
        this._setF(Math.min(1, 1 - (d + F_OFFSET) / F_RANGE));
        if (!this._secure && 1 - this._f >= SWIPE_FINISH)
            this._goingAway();
    }

    _release(down, x, y, lifted) {
        if (this.bouncerShowing) {
            const dragged = y - down.y;
            if (lifted && dragged > DISMISS_SWIPE && !this._bouncer?.ownsPoint(px(down.x), px(down.y)))
                this._hideBouncer(true);
            else
                this._springBouncerBack();
            return;
        }
        if (!this._tracking)
            return;
        this._tracking = false;
        const v = this._tracker.velocityY;
        const up = lifted && (Math.abs(v) >= MIN_FLING_VELOCITY ? v < 0 : this._f < 0.5);
        if (!this._secure) {
            if (up)
                this._goingAway();
            else
                this._settleTo(1, CANCEL_MS * 2);
            return;
        }
        if (up)
            this._settleTo(0, SETTLE_MS, () => this._bouncerShown());
        else
            this._settleTo(1, CANCEL_MS, () => this._dropBouncer());
    }

    _springBouncerBack() {
        const from = this._bouncerBox.translation_y;
        if (!from)
            return;
        animate(this._bouncerBox, {from, to: 0, duration: 150, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._bouncerBox.translation_y = v;
            }});
    }

    _settleTo(f, ms, onDone) {
        this._settle?.stop();
        const from = this._f;
        this._settle = animate(this.actor, {from, to: f, duration: ms * Math.min(1, Math.abs(f - from) / 0.5),
            interpolator: FAST_OUT_SLOW_IN, onUpdate: v => this._setF(v),
            onDone: () => {
                this._settle = null;
                onDone?.();
                this._atRest();
            }});
    }

    // The panel's expansion f: the lock screen's content, the bouncer's progress, the blur and the tint
    _setF(f) {
        this._f = f;
        if (!this._secure) {
            // dismissing: the content fades out by 18 % and rises
            this._content.opacity = Math.round(255 * clamp((f - 0.82) / 0.18));
            this._content.translation_y = px(-SWIPE_RISE * (1 - f));
            return;
        }
        const p = toBouncerProgress(f);
        this._content.opacity = Math.round(255 * (1 - clamp(p / 0.5)));
        this._content.translation_y = 0;
        this._setBackdrop(p);
        if (!this._bouncerActive)
            return;
        const sp = showBouncerProgress(f);
        this._bouncerBox.opacity = Math.round(255 * (1 - sp));
        this._bouncerBox.translation_y = px(BOUNCER_SHIFT * sp);
        if (sp < 1 && !this._appeared) {
            this._appeared = true;
            this._bouncer.appear();
        }
    }

    _setBackdrop(p) {
        const blur = Math.trunc(lerp(0, BLUR_MAX, p));
        this._blur.enabled = blur > 0;
        wholeFrames(this, blur > 0);
        if (blur > 0 && this._blur.radius !== blur)
            this._blur.radius = blur;
        this._scrim.opacity = Math.round(255 * p);
    }

    // The bouncer is built once and kept (a new one only when the lock type changes), and built when the phone locks,
    // not when a swipe starts: building it and drawing its canvases for the first time cost the swipe's first 100 ms.
    // Painted for a few frames at opacity 1/255 as it is built, so every canvas in it is drawn then, under the screen
    // going off, and then hidden: hidden widgets keep their drawings.
    _prepareBouncer() {
        const kind = this._lockType === 'pattern' ? 'pattern' : 'pin';
        this._pinButton.setKind(kind);
        if (this._bouncer?.kind === kind || !this._secure)
            return;
        if (this._bouncerActive)
            return;
        this._bouncer?.destroy();
        this._bouncer = kind === 'pattern' ? new PatternView() : new PinView();
        this._strings = Strings[this._bouncer.kind];
        this._bouncer.connect('submit', (view, secret) => this._submit(secret, view.autoConfirmed));
        this._bouncerBox.add_child(this._bouncer);
        this._bouncer.setMessage(this._strings.prompt, '');
        this._bouncer.setEnabled(false);
        this._bouncerBox.translation_y = 0;
        this._bouncerBox.opacity = 1;
        this._bouncerBox.show();
        let frames = 0;
        this._prerenderId = global.stage.connect('after-paint', () => {
            if (++frames < PRERENDER_FRAMES)
                return;
            this._endPrerender();
        });
    }

    _endPrerender() {
        if (!this._prerenderId)
            return;
        global.stage.disconnect(this._prerenderId);
        this._prerenderId = 0;
        if (!this._bouncerActive) {
            this._bouncerBox.hide();
            this._bouncerBox.opacity = 0;
            this._bouncer?.setEnabled(true);
        }
    }

    _ensureBouncer() {
        if (this._bouncerActive)
            return;
        this._endPrerender();
        this._prepareBouncer();
        this._bouncerActive = true;
        this._autoSuspended = false;
        this._syncAutoConfirm();
        this._bouncerBox.translation_y = px(BOUNCER_SHIFT);
        this._bouncerBox.opacity = 0;
        this._bouncerBox.show();
        this._appeared = false;
        this._showPrompt();
        this._syncLockout();
    }

    _dropBouncer() {
        this._bouncerActive = false;
        this._bouncer?.park();
        this._bouncerBox.hide();
        this._bouncerBox.translation_y = 0;
        this._appeared = false;
        if (this.bouncerShowing) {
            this.bouncerShowing = false;
            this.emit('changed');
        }
        this._atRest();
    }

    // auto-confirm on while the last PIN that unlocked was 6 digits or more, and off for the rest of this bouncer once
    // a PIN it confirmed was wrong: then the PIN may have changed, and only enter can take one of another length
    _syncAutoConfirm() {
        const length = this._pinLength >= MIN_AUTO_CONFIRM && !this._autoSuspended ? this._pinLength : 0;
        if (this._bouncer)
            this._bouncer.autoConfirm = length;
    }

    _bouncerShown() {
        if (this.bouncerShowing)
            return;
        this.bouncerShowing = true;
        this.emit('changed');
    }

    // the lock icon: the bouncer, or with no secret the unlock
    _lockIconTapped() {
        if (!this._secure) {
            if (!this.showing || this.bouncerShowing || this._tracking || this._unlockingNow)
                return;
            this._goingAway();
            return;
        }
        this._pinTapped();
    }

    // The way to the secret: the button over a window that shows over the lock screen, and the lock icon on the lock
    // screen itself. The lock screen takes the front back for the bouncer, and the window comes back when it goes.
    _pinTapped() {
        if (!this.showing || this.bouncerShowing || this._tracking || this._unlockingNow || !this._secure)
            return;
        this._revealStart();
        this._ensureBouncer();
        this._settleTo(0, SETTLE_MS * 2, () => this._bouncerShown());
    }

    // the back gesture: the bouncer goes, the lock screen comes back
    back() {
        if (this.bouncerShowing && !this._unlockingNow)
            this._hideBouncer(true);
    }

    // PRIMARY_BOUNCER -> LOCKSCREEN: the bouncer fades, the lock screen comes back in 250 ms, the blur clears in 450
    _hideBouncer(animated) {
        this._settle?.stop();
        this._settle = null;
        if (!this._bouncerActive) {
            this._setF(1);
            this._atRest();
            return;
        }
        if (!animated) {
            this._setF(1);
            this._dropBouncer();
            return;
        }
        const content = this._content.opacity / 255, box = this._bouncerBox.opacity / 255;
        const start = this._f;
        this._settle = animate(this.actor, {from: 0, to: BOUNCER_TO_LOCK_MS, duration: BOUNCER_TO_LOCK_MS,
            interpolator: LINEAR,
            onUpdate: ms => {
                this._content.opacity = Math.round(255 * lerp(content, 1, clamp(ms / LOCK_ALPHA_BACK_MS)));
                this._bouncerBox.opacity = Math.round(255 * box * (1 - clamp(ms / GONE_SHORT_MS)));
                this._setBackdrop(toBouncerProgress(start) * (1 - ms / BOUNCER_TO_LOCK_MS));
            },
            onDone: () => {
                this._settle = null;
                this._f = 1;
                this._dropBouncer();
                this._setBackdrop(0);
            }});
    }

    // --- the secret

    _showPrompt() {
        if (!this._bouncerActive)
            return;
        this._bouncer.setMessage(this._strings.prompt, this._afterRestart ? this._strings.restart : '');
    }

    // @secret: what was entered, or null for any input (the message goes back to the prompt); @auto: a PIN that
    // confirmed itself
    async _submit(secret, auto = false) {
        if (secret === null) {
            if (!this._lockedOut && !this._checking)
                this._showPrompt();
            return;
        }
        if (this._lockedOut) {
            this._bouncer?.setEnabled(false);
            return;
        }
        // an entry of 3 characters or fewer is no attempt
        if (secret.length <= 3) {
            this._bouncer?.wrong();
            this._bouncer?.setEnabled(true);
            return;
        }
        this._checking = true;
        const result = await this._check(secret);
        this._checking = false;
        if (!this._bouncerActive)
            return;
        if (result === 0) {
            this._failures = 0;
            this._fingerMisses = 0;
            this._lockedUntil = 0;
            // the length auto-confirm goes by: this PIN's, if it is digits only and long enough
            if (this._bouncer.kind === 'pin')
                this._pinLength = /^[0-9]+$/.test(secret) && secret.length >= MIN_AUTO_CONFIRM ? secret.length : 0;
            this._saveThrottle();
            this._afterRestart = false;
            this._noteUnlock('pin');
            // the phone has had its secret: the window over the lock screen may come back (whenlocked.js)
            this._syncSecretRequired();
            this._bouncer.disappear(() => this._goingAway());
            return;
        }
        if (result === 2) {
            // PAM could not run: no attempt counted
            this._bouncer.setMessage('Could not check', 'Try again');
            this._bouncer.wrong();
            this._bouncer.setEnabled(true);
            return;
        }
        this._failures++;
        if (auto) {
            this._autoSuspended = true;
            this._syncAutoConfirm();
        }
        const timeout = lockoutSeconds(this._failures);
        this._lockedUntil = timeout ? Date.now() + timeout * 1000 : 0;
        this._saveThrottle();
        this._bouncer.wrong();
        if (timeout) {
            this._syncLockout();
        } else {
            this._bouncer.setMessage(this._strings.wrong, '');
            this._bouncer.setEnabled(true);
        }
    }

    _check(secret) {
        return checkPin(secret);
    }

    get _lockedOut() {
        return this._lockedUntil > Date.now();
    }

    // a lockout counts down a second at a time, the input off meanwhile
    _syncLockout() {
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._countdownId = 0;
        // the sensor is off while the PIN's own timeout runs
        this._syncFinger();
        if (!this._bouncerActive)
            return;
        if (!this._lockedOut) {
            this._bouncer.setEnabled(true);
            return;
        }
        this._bouncer.setEnabled(false);
        const tick = () => {
            const left = Math.round((this._lockedUntil - Date.now()) / 1000);
            if (left <= 0 || !this._bouncerActive) {
                this._countdownId = 0;
                // the timeout is over: the sensor may hold the device again
                this._syncFinger();
                this._showPrompt();
                this._bouncer?.setEnabled(true);
                return GLib.SOURCE_REMOVE;
            }
            this._bouncer.setMessage(tryAgainIn(left), this._strings.tooMany);
            return GLib.SOURCE_CONTINUE;
        };
        tick();
        this._countdownId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, tick);
    }

    get _throttleFile() {
        return Gio.File.new_for_path(GLib.build_filenamev([GLib.get_user_state_dir(), 'gnome-phone', 'keyguard.json']));
    }

    _loadThrottle() {
        try {
            const [, bytes] = this._throttleFile.load_contents(null);
            const {failures, until, pinLength} = JSON.parse(new TextDecoder().decode(bytes));
            this._failures = failures | 0;
            this._lockedUntil = Number(until) || 0;
            this._pinLength = pinLength | 0;
        } catch (e) {
        }
    }

    async _saveThrottle() {
        const file = this._throttleFile;
        try {
            file.get_parent().make_directory_with_parents(null);
        } catch (e) {
        }
        try {
            const data = JSON.stringify({failures: this._failures, until: this._lockedUntil,
                pinLength: this._pinLength});
            await file.replace_contents_async(new TextEncoder().encode(data), null, false,
                Gio.FileCreateFlags.PRIVATE | Gio.FileCreateFlags.REPLACE_DESTINATION, null);
        } catch (e) {
            logError(e, 'gnome-phone: keyguard state');
        }
    }

    // --- the unlock: the keyguard goes, the screen behind comes up from 95 % (keyguard-extras §D.3-D.5)

    // The shell's "unlock-dialog" mode, left: only while it is still the current one. GNOME's own screen shield,
    // locking alongside on a suspend or a logind Lock, pops the same mode when it unlocks; popping it a second time
    // threw, the unlock stopped half way and the right PIN left a black screen.
    _leaveLockMode() {
        if (this._modeLocked && Main.sessionMode.currentMode === 'unlock-dialog')
            Main.sessionMode.popMode('unlock-dialog');
        this._modeLocked = false;
    }

    _goingAway() {
        if (this._unlockingNow || !this.showing)
            return;
        this._unlockingNow = true;
        this._stopFinger();
        // the hint's layer is not the view's child (it outlives the view while a window is over the lock screen)
        this.hintLayer.hide();
        // and the way to the secret goes with the lock screen, before the unlock animation
        this._pinButton.visible = false;
        this._settle?.stop();
        this._settle = null;
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._countdownId = 0;

        // the wallpaper under the windows now: the launcher draws the same one, apps come up over it
        const monitor = Main.layoutManager.primaryMonitor;
        const under = new St.Widget({x: monitor.x, y: monitor.y, width: monitor.width, height: monitor.height,
            style: this._wallpaperStyle});
        Main.layoutManager.uiGroup.insert_child_below(under, global.window_group);
        this._wallpaper.hide();

        const group = global.window_group;
        const redirect = group.offscreen_redirect;
        group.offscreen_redirect = Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY;
        group.set_pivot_point(0.5, SURFACE_PIVOT_Y);
        const surface = a => {
            group.opacity = Math.round(255 * a);
            group.scale_x = group.scale_y = lerp(SURFACE_SCALE, 1, a);
            group.translation_y = px(SURFACE_SHIFT * (1 - a));
        };
        surface(0);
        this._leaveLockMode();

        const content = this._content.opacity / 255, box = this._bouncerBox.opacity / 255;
        const blur = this._blur.enabled ? this._blur.radius : 0, scrim = this._scrim.opacity / 255;
        const end = SURFACE_DELAY_MS + SURFACE_MS;
        this._unlocking = animate(this.actor, {from: 0, to: Math.max(GONE_MS, end), duration: Math.max(GONE_MS, end),
            interpolator: LINEAR,
            onUpdate: ms => {
                const short = clamp(ms / GONE_SHORT_MS);
                this._content.opacity = Math.round(255 * content * (1 - clamp(ms / (this._secure ? 50 : 126))));
                this._bouncerBox.opacity = Math.round(255 * box * (1 - short));
                const radius = Math.trunc(blur * (1 - short));
                this._blur.enabled = radius > 0;
                wholeFrames(this, radius > 0);
                if (radius > 0)
                    this._blur.radius = radius;
                this._scrim.opacity = Math.round(255 * scrim * (1 - EMPHASIZED_ACCELERATE(clamp(ms / GONE_MS))));
                surface(TOUCH_RESPONSE(clamp((ms - SURFACE_DELAY_MS) / SURFACE_MS)));
            },
            onDone: () => {
                this._unlocking = null;
                surface(1);
                group.set_pivot_point(0, 0);
                group.offscreen_redirect = redirect;
                under.destroy();
                this._finishUnlock();
            }});
    }

    // A finger while a window shows over the lock screen: the phone unlocks behind it, with no reveal and no
    // animation, and the window is the front again (Android's showWhenLocked app: the unlock happens where the user is)
    _unlockOver() {
        this._unlockingNow = true;
        this._stopFinger();
        this.occludedBy = null;
        this._showHiddenWindows();
        this._leaveLockMode();
        this._finishUnlock();
    }

    _finishUnlock() {
        this._unlockingNow = false;
        // the window that showed over the lock screen stays where it is, and is the front again
        this._revealing = false;
        this.occludedBy = null;
        this._showHiddenWindows();
        this._lockedDown = false;
        this._dropBouncer();
        this._setBackdrop(0);
        this._f = 1;
        this.actor.hide();
        this._applyOcclusion();
        this._wallpaper.show();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this.showing = false;
        setLockedHint(false);
        this._syncFingerHint();
        this._syncSecretRequired();
        this.emit('changed');
    }

    // for development (gp-eval): the lock screen goes as if the secret was right
    unlockForDevelopment() {
        if (!this.showing)
            return;
        this._afterRestart = false;
        this._goingAway();
    }

    destroy() {
        offRestyle(this);
        this._stopFinger();
        this._fingerSensor?.stop();
        this._settle?.stop();
        this._unlocking?.stop();
        wholeFrames(this, false);
        if (this._prerenderId)
            global.stage.disconnect(this._prerenderId);
        this._prerenderId = 0;
        this._neo?.disconnectObject(this);
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._wallClock.disconnectObject(this);
        this._interface.disconnectObject(this);
        this._background.disconnectObject(this);
        this._notificationSettings.disconnectObject(this);
        this._state.disconnectObject(this);
        Main.messageTray.disconnectObject(this);
        for (const source of Main.messageTray.getSources())
            source.disconnectObject(this);
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this._revealing = false;
        this.occludedBy = null;
        this._showHiddenWindows();
        this._leaveLockMode();
        const group = global.window_group;
        group.opacity = 255;
        group.scale_x = group.scale_y = 1;
        group.translation_y = 0;
        this.actor.destroy();
        this.hintLayer?.destroy();
    }
}
