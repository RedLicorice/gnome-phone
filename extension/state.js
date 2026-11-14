// What the status bar, the shade header and the quick settings tiles show, read from the system's services
// (NetworkManager, ModemManager, UPower, BlueZ, gsd's rfkill, GSettings), and the toggles the tiles flip.
// One instance; 'changed' is coalesced to at most once per frame.
import 'gi://GnomeBluetooth?version=3.0';

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeBluetooth from 'gi://GnomeBluetooth';
import GObject from 'gi://GObject';
import GUdev from 'gi://GUdev';
import Meta from 'gi://Meta';
import NM from 'gi://NM';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {BroadbandModem} from 'resource:///org/gnome/shell/misc/modemManager.js';
import {getRfkillManager} from 'resource:///org/gnome/shell/ui/status/rfkill.js';

import {Brightness} from './brightness.js';
import {CameraWatch} from './camera.js';
import {setUserOrientationLock, userOrientationLock} from './rotation.js';

Gio._promisify(NM.Client, 'new_async');

const UPOWER_BUS = 'org.freedesktop.UPower';
const UPOWER_PATH = '/org/freedesktop/UPower';
const DISPLAY_DEVICE = '/org/freedesktop/UPower/devices/DisplayDevice';
const UPOWER_DEVICE = 'org.freedesktop.UPower.Device';
const PPD_BUS = 'net.hadess.PowerProfiles';
const PPD_PATH = '/net/hadess/PowerProfiles';
// The torch is the LED the kernel names for the job (devicename:colour:function, the function "torch" or "flash"),
// found as gmobile and Phosh find it, so any phone with a flash LED driver has one. The tile lights it at a quarter
// of its range: the S10e's stock torch default, 60 mA of its 240 (3 of leds-s2mpb02's 12), and never below what
// gmobile's hwdb says still shows (GM_TORCH_MIN_BRIGHTNESS).
const TORCH_FUNCTIONS = ['torch', 'flash'];
const TORCH_SHARE = 0.25;

// the torch LED as {name, level}, or null for a phone without one
function findTorch() {
    const leds = new GUdev.Client({subsystems: ['leds']}).query_by_subsystem('leds');
    for (const fn of TORCH_FUNCTIONS) {
        const led = leds.find(d => d.get_name().split(':').at(-1) === fn);
        if (!led)
            continue;
        const max = led.get_sysfs_attr_as_int('max_brightness');
        if (max <= 0)
            continue;
        const min = parseInt(led.get_property('GM_TORCH_MIN_BRIGHTNESS') ?? '1') || 1;
        return {name: led.get_name(), level: Math.min(max, Math.max(min, Math.round(max * TORCH_SHARE)))};
    }
    return null;
}

// UPower device states that mean "plugged in", until UPower's OnBattery is known: on a charger that gives about what
// the phone takes (the S10e's 500 mA with the screen on), the battery's state flips between charging and discharging
// every few seconds, and Android's bolt is there as long as the charger is (BatteryControllerImpl.mPluggedIn)
const UP_CHARGING = 1;
const UP_FULLY_CHARGED = 4;
const UP_PENDING_CHARGE = 5;

// How fast it charges, from the power the charger is allowed to give: the negotiated voltage (a PD contract's 9 V)
// times the input limit the charger chip was set to, as Android's BatteryStatus does it (max current x max voltage of
// the plugged supply), with Samsung's names on top. Slowly below 7.5 W (a computer's USB port is 2.5 W), fast from
// 12 W, super fast from 20 W. The S10e itself takes 15 W at most (stock's cap, the max77705 driver's PD_MAX_MW), so a
// 27 W charger shows "Fast charging" there, as on One UI.
const POWER_SUPPLY = '/sys/class/power_supply';
const CHARGE_SLOW_W = 7.5;
const CHARGE_FAST_W = 12;
const CHARGE_SUPER_FAST_W = 20;
// a plug-in brings a burst of uevents while the PD contract is made (500 mA first, the 9 V limit ~1 s later)
const CHARGE_SETTLE_MS = 400;

function readSysfs(path) {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]).trim();
    } catch (e) {
        return null;
    }
}

// what the lock screen and the AOD say after the percentage: AOSP's "Charging slowly" and "Charging", One UI's
// "Fast charging" and "Super fast charging"; then, while the battery takes some, how much (the owner asked, 11-08)
export function chargingText(speed, amps = null) {
    const text = {slow: 'Charging slowly', fast: 'Fast charging', superfast: 'Super fast charging'}[speed] ?? 'Charging';
    return amps >= 0.05 ? `${text} • ${amps.toFixed(1)} A` : text;
}

// the current into the battery in A: its fuel gauge's current_now, positive while it charges (the S10e's max170xx),
// or null. Not the charger's input limit, which is only a cap.
function readChargeCurrent() {
    let dir;
    try {
        dir = GLib.Dir.open(POWER_SUPPLY, 0);
    } catch (e) {
        return null;
    }
    let amps = null;
    for (let name; amps === null && (name = dir.read_name()) !== null;) {
        if (readSysfs(`${POWER_SUPPLY}/${name}/type`) !== 'Battery')
            continue;
        const ua = parseInt(readSysfs(`${POWER_SUPPLY}/${name}/current_now`) ?? '');
        if (!isNaN(ua))
            amps = ua / 1e6;
    }
    dir.close();
    return amps;
}

// 'slow', 'normal', 'fast', 'superfast', or null when nothing is plugged in (or the supply says too little to tell)
function readChargeSpeed() {
    let volts = 0, limit = 0, max = 0, plugged = false;
    let dir;
    try {
        dir = GLib.Dir.open(POWER_SUPPLY, 0);
    } catch (e) {
        return null;
    }
    for (let name; (name = dir.read_name()) !== null;) {
        const at = attr => readSysfs(`${POWER_SUPPLY}/${name}/${attr}`);
        if (at('type') === 'Battery' || at('online') !== '1')
            continue;
        plugged = true;
        volts = Math.max(volts, parseInt(at('voltage_now') ?? at('voltage_max') ?? '0') || 0);
        limit = Math.max(limit, parseInt(at('input_current_limit') ?? '0') || 0);
        max = Math.max(max, parseInt(at('current_max') ?? '0') || 0);
    }
    dir.close();
    if (!plugged)
        return null;
    // the charger chip's limit is what the phone draws; a port's current_max is only what it offers
    const amps = (limit || max) / 1e6;
    if (!amps)
        return null;
    const watts = (volts ? volts / 1e6 : 5) * amps;
    return watts >= CHARGE_SUPER_FAST_W ? 'superfast' : watts >= CHARGE_FAST_W ? 'fast'
        : watts >= CHARGE_SLOW_W ? 'normal' : 'slow';
}

// Android's 5 Wi-Fi levels from NetworkManager's 0..100 strength (WifiManager.calculateSignalLevel, roughly)
function wifiLevel(strength) {
    if (strength >= 80)
        return 4;
    if (strength >= 55)
        return 3;
    if (strength >= 30)
        return 2;
    if (strength >= 5)
        return 1;
    return 0;
}

// The 4-bar mobile level from ModemManager's 0..100 quality
function mobileLevel(quality) {
    if (quality >= 70)
        return 4;
    if (quality >= 50)
        return 3;
    if (quality >= 30)
        return 2;
    if (quality >= 10)
        return 1;
    return 0;
}

export const SystemState = GObject.registerClass({
    Signals: {'changed': {}},
}, class SystemState extends GObject.Object {
    constructor() {
        super();
        this.wifi = {available: false, enabled: false, connected: false, level: 0, name: ''};
        this.mobile = {present: false, level: 0, operator: '', tech: '', dataConnected: false, dataEnabled: false};
        this.battery = {present: false, level: 0, plugged: false, speed: null, amps: null,
            icon: 'battery-missing-symbolic'};
        this.powerSave = null;          // null: no power-profiles-daemon
        this.bluetooth = {available: false, powered: false, connected: false, name: ''};
        this.airplane = false;
        this.torch = null;              // null: no torch LED
        this._torch = findTorch();
        // the camera indicator's facts (minerva-camwatch): the chip in the status bar, the sheet a tap opens
        this.camera = new CameraWatch();

        this._cancellable = new Gio.Cancellable();
        this._changedId = 0;

        this._notifications = new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'});
        this._interface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        this._location = new Gio.Settings({schema_id: 'org.gnome.system.location'});
        this._touchscreen = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.peripherals.touchscreen'});
        this._power = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.power'});
        this._color = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.color'});
        for (const settings of [this._notifications, this._interface, this._location, this._touchscreen,
            this._power, this._color])
            settings.connectObject('changed', () => this._changed(), this);
        Main.brightnessManager.globalScale?.connectObject('notify::value', () => this._changed(), this);
        // no brightness pop-up, as on Android: the shade's slider is the only one. The shell's manager shows its OSD
        // for every change it is asked for, the slider's included (BrightnessManager._sync)
        Main.brightnessManager._showOSD = () => {};
        // saving that works without an EDID, a slider with room at the dark end, auto from the lux
        this._brightness = new Brightness();

        this._initNetwork();
        this._initBattery();
        this._initBluetooth();
        this._initRfkill();
        this._syncTorch();
    }

    // coalesce bursts of property changes into one 'changed' before the next frame
    _changed() {
        if (this._changedId)
            return;
        const laters = global.compositor.get_laters();
        this._changedId = laters.add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._changedId = 0;
            this.emit('changed');
            return GLib.SOURCE_REMOVE;
        });
    }

    // --- NetworkManager, ModemManager

    async _initNetwork() {
        try {
            this._client = await NM.Client.new_async(this._cancellable);
        } catch (e) {
            if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                console.warn(`gnome-phone: no NetworkManager: ${e.message}`);
            return;
        }
        this._client.connectObject(
            'notify::wireless-enabled', () => this._syncNetwork(),
            'notify::wwan-enabled', () => this._syncNetwork(),
            'notify::primary-connection', () => this._syncNetwork(),
            'notify::active-connections', () => this._syncNetwork(),
            'device-added', () => this._syncDevices(),
            'device-removed', () => this._syncDevices(),
            this);
        this._syncDevices();
    }

    _syncDevices() {
        this._wifiDevice?.disconnectObject(this);
        this._wifiDevice = this._client.get_devices().find(d => d instanceof NM.DeviceWifi) ?? null;
        this._wifiDevice?.connectObject(
            'notify::active-access-point', () => this._syncAccessPoint(),
            'state-changed', () => this._syncNetwork(),
            this);
        this._syncAccessPoint();

        const modem = this._client.get_devices().find(d => d instanceof NM.DeviceModem) ?? null;
        if (modem !== this._modemDevice) {
            this._modemDevice?.disconnectObject(this);
            this._modem?.disconnectObject(this);
            this._modemDevice = modem;
            this._modem = null;
            if (modem) {
                modem.connectObject('state-changed', () => this._syncNetwork(), this);
                try {
                    this._modem = new BroadbandModem(modem.udi, modem.current_capabilities);
                    this._modem.connectObject(
                        'notify::signal-quality', () => this._syncNetwork(),
                        'notify::operator-name', () => this._readOperatorNames(),
                        'notify::access-technology', () => this._syncNetwork(),
                        this);
                } catch (e) {
                    console.warn(`gnome-phone: no modem: ${e.message}`);
                }
            }
            this._readOperatorNames();
        }
        this._syncNetwork();
    }

    // The mobile network's name, as Android has it. A network that sends no name of its own leaves ModemManager
    // with its code ("26203"), and the shell's BroadbandModem then picks a name from mobile-broadband-provider-info,
    // which lists several operators under one code: it said "sipgate" for a LycaMobile SIM. So, read from
    // ModemManager directly: the SIM's own name for its operator (its SPN, what Android shows at home), and the
    // network's name while roaming, when it has a real one.
    async _readOperatorNames() {
        const path = this._modemDevice?.udi;
        if (!path?.startsWith('/org/freedesktop/ModemManager1/')) {
            this._simOperator = this._networkOperator = '';
            return;
        }
        const get = async (object, iface, prop) => {
            try {
                const reply = await Gio.DBus.system.call('org.freedesktop.ModemManager1', object,
                    'org.freedesktop.DBus.Properties', 'Get', new GLib.Variant('(ss)', [iface, prop]),
                    null, Gio.DBusCallFlags.NONE, 2000, this._cancellable);
                return reply.deepUnpack()[0].deepUnpack();
            } catch {
                return null;
            }
        };
        const sim = await get(path, 'org.freedesktop.ModemManager1.Modem', 'Sim');
        const spn = sim && sim !== '/' ? await get(sim, 'org.freedesktop.ModemManager1.Sim', 'OperatorName') : null;
        const network = await get(path, 'org.freedesktop.ModemManager1.Modem.Modem3gpp', 'OperatorName');
        const registration = await get(path, 'org.freedesktop.ModemManager1.Modem.Modem3gpp', 'RegistrationState');
        if (path !== this._modemDevice?.udi)
            return;             // another modem by now
        this._simOperator = spn ?? '';
        // a name, not the code; MMModem3gppRegistrationState 5 is roaming
        this._networkOperator = network && !/^\d+$/.test(network) ? network : '';
        this._roaming = registration === 5;
        this._syncNetwork();
    }

    // home: the SIM's name, else the network's; roaming: the network's first. The shell's guess only when neither
    // has one.
    _operatorName() {
        const sim = this._simOperator ?? '', network = this._networkOperator ?? '';
        const name = this._roaming ? network || sim : sim || network;
        return name || this._modem?.operator_name || '';
    }

    _syncAccessPoint() {
        this._accessPoint?.disconnectObject(this);
        this._accessPoint = this._wifiDevice?.active_access_point ?? null;
        this._accessPoint?.connectObject('notify::strength', () => this._syncNetwork(), this);
        this._syncNetwork();
    }

    _syncNetwork() {
        const client = this._client;
        const wifiState = this._wifiDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        const ap = this._accessPoint;
        let name = '';
        if (ap?.ssid)
            name = NM.utils_ssid_to_utf8(ap.ssid.get_data());
        this.wifi = {
            available: !!this._wifiDevice,
            enabled: client.wireless_enabled,
            connected: wifiState === NM.DeviceState.ACTIVATED && !!ap,
            level: ap ? wifiLevel(ap.strength) : 0,
            name,
        };

        const modemState = this._modemDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        const tech = this._modem?.accessTechnology ?? '';
        this.mobile = {
            present: !!this._modemDevice,
            level: mobileLevel(this._modem?.signal_quality ?? 0),
            operator: this._operatorName(),
            tech: tech === 'unknown' || tech === 'other' ? '' : tech,
            dataConnected: modemState === NM.DeviceState.ACTIVATED,
            dataEnabled: client.wwan_enabled && modemState === NM.DeviceState.ACTIVATED,
        };
        this._changed();
    }

    setWifiEnabled(enabled) {
        this._client?.dbus_set_property(NM.DBUS_PATH, NM.DBUS_INTERFACE, 'WirelessEnabled',
            new GLib.Variant('b', enabled), -1, null, null);
    }

    // Mobile data: (de)activate the modem's connection
    setMobileData(enabled) {
        const device = this._modemDevice;
        if (!device)
            return;
        if (!enabled) {
            device.disconnect_async(null, null);
            return;
        }
        if (!this._client.wwan_enabled) {
            this._client.dbus_set_property(NM.DBUS_PATH, NM.DBUS_INTERFACE, 'WwanEnabled',
                new GLib.Variant('b', true), -1, null, null);
        }
        const connection = device.get_available_connections()[0] ?? null;
        this._client.activate_connection_async(connection, device, null, null, null);
    }

    // --- UPower, power-profiles-daemon

    _initBattery() {
        // UPower says nothing when only the charger's limit changes (a PD contract after the plug-in): the kernel's
        // power_supply uevents do
        this._chargeSpeed = readChargeSpeed();
        this._chargeSpeedId = 0;
        this._udev = new GUdev.Client({subsystems: ['power_supply']});
        this._udev.connectObject('uevent', () => {
            if (this._chargeSpeedId)
                return;
            this._chargeSpeedId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CHARGE_SETTLE_MS, () => {
                this._chargeSpeedId = 0;
                const speed = readChargeSpeed();
                // a new contract or limit: the speed, and while plugged in the battery's current, read again
                if (speed !== this._chargeSpeed || this.battery.plugged) {
                    this._chargeSpeed = speed;
                    this._syncBattery();
                }
                return GLib.SOURCE_REMOVE;
            });
        }, this);
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            UPOWER_BUS, DISPLAY_DEVICE, UPOWER_DEVICE, this._cancellable, (o, res) => {
                try {
                    this._upower = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._upower.connectObject('g-properties-changed', () => this._syncBattery(), this);
                this._syncBattery();
            });
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            UPOWER_BUS, UPOWER_PATH, UPOWER_BUS, this._cancellable, (o, res) => {
                try {
                    this._upowerDaemon = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._upowerDaemon.connectObject('g-properties-changed', () => this._syncBattery(), this);
                this._syncBattery();
            });
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            PPD_BUS, PPD_PATH, PPD_BUS, this._cancellable, (o, res) => {
                try {
                    this._ppd = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._ppd.connectObject('g-properties-changed', () => this._syncPowerSave(), this);
                this._syncPowerSave();
            });
    }

    _syncBattery() {
        if (!this._upower)
            return;
        const prop = name => this._upower.get_cached_property(name)?.unpack();
        const onBattery = this._upowerDaemon?.get_cached_property('OnBattery')?.unpack();
        const plugged = onBattery !== undefined ? !onBattery
            : [UP_CHARGING, UP_FULLY_CHARGED, UP_PENDING_CHARGE].includes(prop('State') ?? 0);
        this.battery = {
            present: !!prop('IsPresent'),
            level: Math.round(prop('Percentage') ?? 0),
            plugged,
            speed: plugged ? this._chargeSpeed : null,
            // read at UPower's every refresh of the battery
            amps: plugged ? readChargeCurrent() : null,
            icon: prop('IconName') || 'battery-missing-symbolic',
        };
        this._changed();
    }

    _syncPowerSave() {
        const profile = this._ppd.get_cached_property('ActiveProfile')?.unpack();
        this.powerSave = profile === undefined ? null : profile === 'power-saver';
        this._changed();
    }

    setPowerSave(enabled) {
        if (this.powerSave === null)
            return;
        this._ppd.call('org.freedesktop.DBus.Properties.Set',
            new GLib.Variant('(ssv)', [PPD_BUS, 'ActiveProfile',
                new GLib.Variant('s', enabled ? 'power-saver' : 'balanced')]),
            Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // --- the torch

    // what the LED is at, read back from sysfs: the tile shows that, not what was asked for
    _syncTorch() {
        if (!this._torch)
            return;
        Gio.File.new_for_path(`/sys/class/leds/${this._torch.name}/brightness`).load_contents_async(this._cancellable,
            (file, res) => {
                try {
                    const [, bytes] = file.load_contents_finish(res);
                    this.torch = parseInt(new TextDecoder().decode(bytes)) > 0;
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    this.torch = null;
                }
                this._changed();
            });
    }

    // logind lets the active session set its seat's LEDs, so no udev rule is needed
    setTorch(on) {
        if (this.torch === null)
            return;
        this.torch = on;
        this._changed();
        Gio.DBus.system.call('org.freedesktop.login1', '/org/freedesktop/login1/session/auto',
            'org.freedesktop.login1.Session', 'SetBrightness',
            new GLib.Variant('(ssu)', ['leds', this._torch.name, on ? this._torch.level : 0]),
            null, Gio.DBusCallFlags.NONE, -1, this._cancellable, (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    console.warn(`gnome-phone: torch: ${e.message}`);
                }
                this._syncTorch();
            });
    }

    // --- Bluetooth, rfkill

    _initBluetooth() {
        this._bt = new GnomeBluetooth.Client();
        this._bt.connectObject(
            'notify::default-adapter-powered', () => this._syncBluetooth(),
            'notify::default-adapter-state', () => this._syncBluetooth(),
            'notify::default-adapter', () => this._syncBluetooth(),
            'device-added', (c, device) => this._watchDevice(device),
            'device-removed', () => this._syncBluetooth(),
            this);
        const devices = this._bt.get_devices();
        for (let i = 0; i < devices.get_n_items(); i++)
            this._watchDevice(devices.get_item(i));
        this._syncBluetooth();
    }

    _watchDevice(device) {
        device.connectObject('notify::connected', () => this._syncBluetooth(), this);
        this._syncBluetooth();
    }

    _syncBluetooth() {
        const devices = this._bt.get_devices();
        let connected = null;
        for (let i = 0; i < devices.get_n_items(); i++) {
            const device = devices.get_item(i);
            if (device.connected) {
                connected = device;
                break;
            }
        }
        this.bluetooth = {
            available: this._bt.default_adapter_state !== GnomeBluetooth.AdapterState.ABSENT,
            powered: this._bt.default_adapter_powered,
            connected: !!connected,
            name: connected?.alias ?? '',
        };
        this._changed();
    }

    setBluetooth(enabled) {
        this._rfkill?._proxy.call('org.freedesktop.DBus.Properties.Set',
            new GLib.Variant('(ssv)', ['org.gnome.SettingsDaemon.Rfkill', 'BluetoothAirplaneMode',
                new GLib.Variant('b', !enabled)]),
            Gio.DBusCallFlags.NONE, -1, null, null);
        if (enabled && !this._bt.default_adapter_powered)
            this._bt.default_adapter_powered = true;
    }

    _initRfkill() {
        this._rfkill = getRfkillManager();
        const sync = () => {
            this.airplane = !!this._rfkill.airplane_mode;
            this._changed();
        };
        this._rfkill.connectObject('notify::airplane-mode', sync, this);
        sync();
    }

    setAirplane(enabled) {
        this._rfkill.airplane_mode = enabled;
    }

    // --- GSettings

    get dnd() {
        return !this._notifications.get_boolean('show-banners');
    }

    set dnd(v) {
        this._notifications.set_boolean('show-banners', !v);
    }

    get darkMode() {
        return this._interface.get_string('color-scheme') === 'prefer-dark';
    }

    set darkMode(v) {
        this._interface.set_string('color-scheme', v ? 'prefer-dark' : 'default');
    }

    get location() {
        return this._location.get_boolean('enabled');
    }

    set location(v) {
        this._location.set_boolean('enabled', v);
    }

    // Android's auto-rotate is the inverse of GNOME's orientation lock, as the user set it (rotation.js may hold the
    // lock for the lock screen and home)
    get autoRotate() {
        return !userOrientationLock(this._touchscreen);
    }

    set autoRotate(v) {
        setUserOrientationLock(this._touchscreen, !v);
        this._changed();
    }

    get nightLight() {
        return this._color.get_boolean('night-light-enabled');
    }

    set nightLight(v) {
        this._color.set_boolean('night-light-enabled', v);
    }

    get autoBrightness() {
        return this._power.get_boolean('ambient-enabled');
    }

    set autoBrightness(v) {
        this._power.set_boolean('ambient-enabled', v);
    }

    // 0..1, the shell's brightness manager (backlight via gsd-power)
    get brightness() {
        return Main.brightnessManager.globalScale?.value ?? null;
    }

    set brightness(v) {
        const scale = Main.brightnessManager.globalScale;
        if (scale)
            scale.value = v;
    }

    destroy() {
        this._cancellable.cancel();
        if (this._changedId)
            global.compositor.get_laters().remove(this._changedId);
        if (this._chargeSpeedId)
            GLib.source_remove(this._chargeSpeedId);
        this._udev?.disconnectObject(this);
        this._udev = null;
        Main.brightnessManager.globalScale?.disconnectObject(this);
        delete Main.brightnessManager._showOSD;
        this._brightness.destroy();
        this.camera.destroy();
        for (const obj of [this._notifications, this._interface, this._location, this._touchscreen, this._power,
            this._color, this._client, this._wifiDevice, this._accessPoint, this._modemDevice, this._modem, this._upower,
            this._upowerDaemon, this._ppd, this._bt, this._rfkill])
            obj?.disconnectObject(this);
        const devices = this._bt.get_devices();
        for (let i = 0; i < devices.get_n_items(); i++)
            devices.get_item(i).disconnectObject(this);
        this._bt = null;
        this._client = null;
    }
});
