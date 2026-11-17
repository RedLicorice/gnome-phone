// Whether the internet is there, as Minerva's link supervisor says it (minerva-net, org.minerva.Network1 on the system
// bus: it checks the internet over the active link, with hysteresis, so the lock screen and the status bar do not
// flicker). No shell imports: it runs under plain gjs too. The words are internet.json's.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

const BUS = 'org.minerva.Network1';
const PATH = '/org/minerva/Network1';
const PROPS = 'org.freedesktop.DBus.Properties';

function loadTexts() {
    try {
        const file = Gio.File.new_for_uri(import.meta.url).get_parent().get_child('internet.json');
        return JSON.parse(new TextDecoder().decode(file.load_contents(null)[1]));
    } catch (e) {
        log(`gnome-phone: internet texts: ${e.message}`);
        return {};
    }
}

export const InternetWatch = GObject.registerClass({
    Signals: {'changed': {}},
}, class InternetWatch extends GObject.Object {
    constructor() {
        super();
        this.known = false;             // false: minerva-net is not on the bus, nothing is claimed
        this.online = true;
        this.text = loadTexts().offline ?? '';
        this._cancellable = new Gio.Cancellable();
        this._subscription = Gio.DBus.system.signal_subscribe(BUS, PROPS, 'PropertiesChanged', PATH, null,
            Gio.DBusSignalFlags.NONE, (c, sender, path, iface, signal, params) => {
                const [, changed] = params.recursiveUnpack();
                if ('Online' in changed)
                    this._set(changed.Online);
            });
        this._watch = Gio.bus_watch_name(Gio.BusType.SYSTEM, BUS, Gio.BusNameWatcherFlags.NONE,
            () => this._fetch(), () => this._set(null));
    }

    // what the lock screen and the status bar show: the word, only while the internet is known to be missing
    get offline() {
        return this.known && !this.online;
    }

    _fetch() {
        Gio.DBus.system.call(BUS, PATH, PROPS, 'Get', new GLib.Variant('(ss)', [BUS, 'Online']),
            new GLib.VariantType('(v)'), Gio.DBusCallFlags.NO_AUTO_START, 2000, this._cancellable, (conn, res) => {
                try {
                    this._set(conn.call_finish(res).recursiveUnpack()[0]);
                } catch (e) {
                    if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        log(`gnome-phone: internet state: ${e.message}`);
                }
            });
    }

    _set(online) {
        const known = online !== null, value = known ? !!online : true;
        if (known === this.known && value === this.online)
            return;
        this.known = known;
        this.online = value;
        this.emit('changed');
    }

    destroy() {
        this._cancellable.cancel();
        Gio.DBus.system.signal_unsubscribe(this._subscription);
        Gio.bus_unwatch_name(this._watch);
    }
});
