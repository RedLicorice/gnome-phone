// GNOME Phone: turns GNOME Shell (mobile) into the phone the Neo launcher is the home screen of.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {Boost} from './boost.js';
import {ScreenshotChord} from './chord.js';
import {CollectWhenStill} from './gc.js';
import {Cutout, CutoutBand} from './cutout.js';
import {Desktop, isDesktopSession} from './dock.js';
import {EndMark} from './endmark.js';
import {HeadsUp} from './headsup.js';
import {Home} from './home.js';
import {KeyHaptics} from './keyhaptics.js';
import {AppSwitchKey} from './keys.js';
import {Keyguard} from './keyguard.js';
import {NoWindowMipmaps} from './mipmaps.js';
import {startLive, stopLive} from './palette.js';
import {AndroidKeyboard} from './osk.js';
import {TouchFocusKeyboard} from './oskfocus.js';
import {TouchKeyboard} from './hkeyboard.js';
import {overWindows} from './scanout.js';
import {Power} from './power.js';
import {PowerMenu} from './powermenu.js';
import {Rotation} from './rotation.js';
import {closeApp, measureApps} from './recents.js';
import {Service} from './service.js';
import {Shade} from './shade.js';
import {Stacks} from './stacks.js';
import {SystemState} from './state.js';
import {CameraSheet} from './camera.js';
import {StatusBar, hideShellPanel} from './statusbar.js';
import {ThemeSwaps} from './theme.js';
import {VolumeDialog} from './volume.js';
import {WhenLocked} from './whenlocked.js';

export default class GnomePhoneExtension extends Extension {
    enable() {
        // ~/.config/gnome-phone/debug: the shell's Eval and Screenshot D-Bus calls are open to every client, for
        // development only
        const debug = GLib.build_filenamev([GLib.get_user_config_dir(), 'gnome-phone', 'debug']);
        if (GLib.file_test(debug, GLib.FileTest.EXISTS))
            global.context.unsafe_mode = true;

        // the look (theme.js, palette.js) follows its settings from here until disable()
        startLive();

        this._keepThroughLock();

        // Android apps' keyboard requests open GNOME's on-screen keyboard, docked as on the phone; without it the rest
        // still comes up
        try {
            this._androidKeyboard = new AndroidKeyboard();
        } catch (e) {
            logError(e, 'gnome-phone: the on-screen keyboard for Android apps');
            this._androidKeyboard = null;
        }
        // a text field a tap focuses opens the keyboard (WebKit's page fields never ask for it themselves)
        try {
            this._touchFocusKeyboard = new TouchFocusKeyboard();
        } catch (e) {
            logError(e, 'gnome-phone: the keyboard for a tapped text field');
            this._touchFocusKeyboard = null;
        }
        // Hacker's Keyboard's full layout and behaviour on the on-screen keyboard; without it the shell's own keys
        try {
            this._touchKeyboard = new TouchKeyboard();
        } catch (e) {
            logError(e, 'gnome-phone: the touch keyboard');
            this._touchKeyboard = null;
        }

        // docked to a display: GNOME's own desktop there, and none of the phone's UI (dock.js)
        if (isDesktopSession()) {
            this._desktop = new Desktop();
            return;
        }

        // the lock screen and home are portrait, the session starts locked, and everything is laid out for an upright
        // panel: a shell started sideways turns upright first
        this._rotation = new Rotation();
        this._rotation.hold();
        this._buildId = this._rotation.whenUpright(() => {
            this._buildId = 0;
            this._build();
        });
    }

    // The GNOME Phone session turns the extension on through its session mode (gnome-phone.json). The unlock-dialog
    // mode the lock screen pushes over it lists no extensions of its own, so the shell would turn this one off at
    // every lock, its lock screen with it, and on again as that pops the mode: a loop. While the session is locked,
    // the extension stays among the mode's. (Turned on in the user's settings instead, it stays on by its
    // session-modes alone.)
    _keepThroughLock() {
        const manager = Main.extensionManager;
        if (!Main.sessionMode.enabledExtensions?.includes(this.uuid) || Object.hasOwn(manager, '_getModeExtensions'))
            return;
        const uuid = this.uuid;
        manager._getModeExtensions = function () {
            const extensions = Object.getPrototypeOf(this)._getModeExtensions.call(this);
            return Main.sessionMode.isLocked && !extensions.includes(uuid) ? [...extensions, uuid] : extensions;
        };
        this._keptThroughLock = true;
    }

    _build() {
        // touch and launch hints for minerva-boost, before the service that launches apps
        this._boost = new Boost();
        this._gc = new CollectWhenStill();
        this._mipmaps = new NoWindowMipmaps();
        this._home = new Home(this);
        this._service = new Service(this._home, this._boost);
        // whose each window is (agents' windows stay in recents), and the API agents open them through
        this._stacks = new Stacks(this._home, closeApp, measureApps);
        this._service.setStacks(this._stacks);

        // Android's SystemUI: the status bar in place of the shell's top panel
        this._state = new SystemState();
        this._restorePanel = hideShellPanel();
        // the camera hole: under the status bar upright, on a short edge turned, where apps keep clear of it
        this._cutout = new Cutout();
        this._statusBar = new StatusBar(this._state, this._home, this._cutout);
        this._cutoutBand = new CutoutBand(this._cutout, this._statusBar);
        Main.layoutManager.addChrome(this._cutoutBand, {affectsStruts: true});
        overWindows(this._cutoutBand);
        Main.layoutManager.addChrome(this._statusBar, {affectsStruts: true});
        overWindows(this._statusBar);
        // ... and the notification shade pulled from it, above everything
        this._shade = new Shade(this._state, this._statusBar);
        Main.layoutManager.addTopChrome(this._shade);
        overWindows(this._shade);
        // heads-up notifications in place of GNOME's banners, over the shade too: it takes them along as it opens
        this._headsUp = new HeadsUp(this._shade);
        Main.layoutManager.addTopChrome(this._headsUp.layer);
        overWindows(this._headsUp.layer);
        // the lock screen, over them all (the heads-ups hold back while it is up)
        this._keyguard = new Keyguard(this._state);
        Main.layoutManager.addTopChrome(this._keyguard.actor);
        overWindows(this._keyguard.actor);
        this._service.setKeyguard(this._keyguard);
        // an app's window over the lock screen: the ringing alarm, a call coming in
        this._stacks.setKeyguard(this._keyguard);
        this._whenLocked = new WhenLocked(this._keyguard, this._stacks);
        this._rotation.follow(this._keyguard, this._home);
        // the volume keys show the volume dialog, over the shade and the lock screen too
        this._volume = new VolumeDialog();
        // ... unless the app in front took them (a ringing timer stops on one)
        this._volume.keysTaker = () => this._service.volumeKeysTaker();
        Main.layoutManager.addTopChrome(this._volume);
        overWindows(this._volume);
        // power and volume down together: a screenshot
        this._chord = new ScreenshotChord();
        this._volume.chord = this._chord;
        // the power menu a long press of the power key opens, over the lock screen too
        this._powerMenu = new PowerMenu({
            secure: () => this._keyguard._secure,
            lockdown: () => this._keyguard.lockdown(),
        });
        Main.layoutManager.addTopChrome(this._powerMenu.layer);
        overWindows(this._powerMenu.layer);
        // the sheet a tap on the status bar's camera chip opens, over the lock screen too
        this._cameraSheet = new CameraSheet(this._state.camera);
        Main.layoutManager.addTopChrome(this._cameraSheet.layer);
        overWindows(this._cameraSheet.layer);
        // the screen off, the AOD or black, over everything: the power key, the double tap, the timeout
        // the optional look's wallpaper and icon pack follow its setting
        this._themeSwaps = new ThemeSwaps();
        this._power = new Power(this._keyguard, this._state, this._powerMenu, this._chord);
        Main.layoutManager.addTopChrome(this._power.layer);
        overWindows(this._power.layer);
        this._service.setPower(this._power);
        // ... and while it is off, the volume keys do nothing (an app that took them still gets them: a call)
        this._volume.screenOff = () => this._power && !this._power.interactive;
        // the Bixby key: recents, with the screen on (the lock screen's own action mode keeps it out there)
        this._appSwitch = new AppSwitchKey(() => {
            if (this._power.interactive)
                this._service.toggleRecents();
        });
        // ... and over them all, the end of uiGroup
        this._endMark = new EndMark();
        // the on-screen keyboard's keys tick under the finger
        this._keyHaptics = new KeyHaptics();
        // The shell's haptics never start feedbackd (NO_AUTO_START), they only find it running: Calls' and Chatty's
        // daemons started it at login, the Phone and Messaging apps in their place only on a ring. So it starts here.
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'StartServiceByName', new GLib.Variant('(su)', ['org.sigxcpu.Feedback', 0]), null,
            Gio.DBusCallFlags.NONE, -1, null, null);
        // locked from the start, as a phone after boot, once the extension system is done with this
        this._lockId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._lockId = 0;
            this._keyguard.lock();
            return GLib.SOURCE_REMOVE;
        });
    }

    disable() {
        if (this._keptThroughLock)
            delete Main.extensionManager._getModeExtensions;
        this._keptThroughLock = false;
        this._androidKeyboard?.destroy();
        this._androidKeyboard = null;
        this._touchKeyboard?.destroy();
        this._touchKeyboard = null;
        this._touchFocusKeyboard?.destroy();
        this._touchFocusKeyboard = null;
        this._desktop?.destroy();
        this._desktop = null;
        this._rotation?.cancelWait(this._buildId);
        this._buildId = 0;
        if (this._lockId)
            GLib.source_remove(this._lockId);
        this._lockId = 0;
        this._keyHaptics?.destroy();
        this._keyHaptics = null;
        this._endMark?.destroy();
        this._endMark = null;
        this._appSwitch?.destroy();
        this._appSwitch = null;
        this._themeSwaps?.destroy();
        this._themeSwaps = null;
        this._power?.destroy();
        this._power = null;
        this._cameraSheet?.destroy();
        this._cameraSheet = null;
        this._powerMenu?.destroy();
        this._powerMenu = null;
        this._volume?.destroy();
        this._volume = null;
        this._chord?.destroy();
        this._chord = null;
        this._whenLocked?.destroy();
        this._whenLocked = null;
        this._keyguard?.destroy();
        this._keyguard = null;
        this._headsUp?.destroy();
        this._headsUp = null;
        this._shade?.destroy();
        this._shade = null;
        this._cutoutBand?.destroy();
        this._cutoutBand = null;
        this._statusBar?.destroy();
        this._statusBar = null;
        this._cutout?.destroy();
        this._cutout = null;
        this._restorePanel?.();
        this._restorePanel = null;
        this._state?.destroy();
        this._state = null;

        this._stacks?.destroy();
        this._stacks = null;
        this._service?.destroy();
        this._service = null;
        this._home?.destroy();
        this._home = null;
        this._mipmaps?.destroy();
        this._mipmaps = null;
        this._gc?.destroy();
        this._gc = null;
        this._rotation?.destroy();
        this._rotation = null;
        this._boost?.destroy();
        this._boost = null;
        // last: the surfaces above drop their own callbacks as they go, this drops what is left and the settings' watch
        stopLive();
    }
}
