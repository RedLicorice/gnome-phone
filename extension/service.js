// de.yesman.GnomePhone on the session bus: what the launcher asks of the shell.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import {LaunchTransition} from './launch.js';
import {Navigation} from './navigation.js';

const IFACE = `<node>
  <interface name="de.yesman.GnomePhone">
    <!-- start an app; from = the icon's rectangle on screen, in pixels, the window grows out of it; icon = the
         launcher's picture of the icon (a PNG, for the splash), or '' for the app's themed icon -->
    <method name="Launch">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="(dddd)" name="from" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
    </method>
    <!-- the icons on the launcher's current page and dock: app id, the icon's box on screen in pixels, its picture
         (a PNG, or '') -->
    <method name="Icons">
      <arg type="a(s(dddd)s)" name="icons" direction="in"/>
    </method>
    <!-- the launcher's app drawer opened (or started to) or closed -->
    <method name="Drawer">
      <arg type="b" name="open" direction="in"/>
    </method>
    <!-- Android's full-screen intent (Notification.Builder.setFullScreenIntent): an app's urgent screen, a ringing alarm
         or a call coming in, starts when the phone is asleep or locked, and the screen comes on for it; with the phone
         in use the app's heads-up notification is enough. app_id: a desktop file id; started: whether it started it.
         Over the lock screen it shows if its desktop file says X-GnomePhone-ShowWhenLocked=true (whenlocked.js) -->
    <method name="LaunchFullScreen">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="b" name="started" direction="out"/>
    </method>
    <!-- Android hands a key to the focused activity first, and a ringing alarm or timer stops on a volume key. Here
         the volume keys are the shell's, so an app asks for them: while a window of app_id (the caller's GApplication
         id) has the focus, a volume key does not change the volume but activates the app's "volume-key" action, with
         1 for up and -1 for down, when it is pressed. take = false gives them back, and so does the caller leaving
         the bus. -->
    <method name="TakeVolumeKeys">
      <arg type="s" name="app_id" direction="in"/>
      <arg type="b" name="take" direction="in"/>
    </method>
  </interface>
</node>`;

export class Service {
    constructor(home, boost) {
        this._home = home;
        this._launch = new LaunchTransition(boost);
        this._nav = new Navigation(home, boost);
        this._nav.onGestureBegin = () => this._launch.cancelSplash();
        this._nav.expectWindow = app => this._launch.expect(app);
        this._volumeKeys = new Map();   // app id -> {sender, watch}: who took the volume keys
        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, '/de/yesman/GnomePhone');
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, 'de.yesman.GnomePhone',
            Gio.BusNameOwnerFlags.REPLACE, null, null, null);
    }

    Launch(appId, from, icon) {
        const app = Shell.AppSystem.get_default().lookup_app(appId);
        if (!app) {
            console.warn(`gnome-phone: no app ${appId}`);
            return;
        }
        this._launch.launch(app, from, icon);
    }

    Icons(icons) {
        this._nav.setIcons(icons);
    }

    Drawer(open) {
        this._nav.drawerOpen = open;
    }

    // stacks.js: recents shows the owners' stacks as tabs
    setStacks(stacks) {
        this._nav.stacks = stacks;
    }

    setKeyguard(keyguard) {
        this._keyguard = keyguard;
        this._nav.setKeyguard(keyguard);
    }

    // power.js's Power: whether the screen is on, and waking it
    setPower(power) {
        this._power = power;
    }

    LaunchFullScreen(appId) {
        if (this._power?.interactive && !this._keyguard?.showing)
            return false;
        // an app id ("de.yesman.Clock.Ringing") or a desktop id ("de.yesman.Clock.Ringing.desktop")
        const appSystem = Shell.AppSystem.get_default();
        const app = appSystem.lookup_app(appId) ?? appSystem.lookup_app(`${appId}.desktop`);
        if (!app) {
            console.warn(`gnome-phone: no app ${appId} to launch full-screen`);
            return false;
        }
        app.activate();
        this._power?.wakeUp('full-screen');
        return true;
    }

    TakeVolumeKeysAsync([appId, take], invocation) {
        this._giveVolumeKeysBack(appId);
        if (take) {
            const sender = invocation.get_sender();
            const watch = Gio.bus_watch_name_on_connection(invocation.get_connection(), sender,
                Gio.BusNameWatcherFlags.NONE, null, () => this._giveVolumeKeysBack(appId));
            this._volumeKeys.set(appId, {sender, watch});
        }
        invocation.return_value(null);
    }

    _giveVolumeKeysBack(appId) {
        const taker = this._volumeKeys.get(appId);
        if (!taker)
            return;
        Gio.bus_unwatch_name(taker.watch);
        this._volumeKeys.delete(appId);
    }

    // volume.js: whether the app in front took the volume keys; if so, a function that hands it one (+1 or -1)
    volumeKeysTaker() {
        const app = Shell.WindowTracker.get_default().focus_app;
        const appId = app?.get_id().replace(/\.desktop$/, '');
        const taker = appId && this._volumeKeys.get(appId);
        if (!taker)
            return null;
        // the GApplication's object path: its id as a path, de.yesman.Clock.Ringing at /de/yesman/Clock/Ringing
        const path = `/${appId.replaceAll('.', '/')}`;
        return direction => Gio.DBus.session.call(taker.sender, path, 'org.freedesktop.Application',
            'ActivateAction', new GLib.Variant('(sava{sv})', ['volume-key', [new GLib.Variant('i', direction)], {}]),
            null, Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // the Bixby key (keys.js AppSwitchKey)
    toggleRecents() {
        this._nav.toggleRecents();
    }

    destroy() {
        this._launch.destroy();
        this._nav.destroy();
        for (const appId of [...this._volumeKeys.keys()])
            this._giveVolumeKeysBack(appId);
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
