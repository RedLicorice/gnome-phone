#!/usr/bin/env python3
"""On the phone, a try of the earpiece by proximity (src/msg-earpiece.c) with the real call audio: a private bus as the
system bus, with a proximity sensor and a ModemManager of its own whose events this script makes; the real callaudiod
and PipeWire route the sound. It checks the default sink and callaudiod's mode after each event:

  near -> earpiece, call mode;  away -> speaker, default mode;  near again -> earpiece;  a call comes -> speaker at
  once, the note stopped;  callaudiod already in call mode (a call) -> near and away leave it alone;  the end -> the
  sensor released, the sound as before.

  usage: try-earpiece.py BUILD_DIR      (messaging.conf [media] earpiece_by_proximity = true)
Changes the phone's sound for a few seconds, and puts it back."""

import os
import select
import subprocess
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

SENSOR = """<node><interface name='net.hadess.SensorProxy'>
  <method name='ClaimProximity'/><method name='ReleaseProximity'/>
  <property name='HasProximity' type='b' access='read'/><property name='ProximityNear' type='b' access='read'/>
</interface></node>"""
MANAGER = """<node><interface name='org.freedesktop.DBus.ObjectManager'>
  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>
  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>
</interface></node>"""
CALL_AUDIO = ["busctl", "--user", "call", "org.mobian_project.CallAudio", "/org/mobian_project/CallAudio",
              "org.mobian_project.CallAudio"]
SETTLE_S = 2.5  # callaudiod's profile switch, then PipeWire's new default sink


def sink() -> str:
    return subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()


def mode() -> int:
    out = subprocess.run(["busctl", "--user", "get-property", "org.mobian_project.CallAudio",
                          "/org/mobian_project/CallAudio", "org.mobian_project.CallAudio", "AudioMode"],
                         capture_output=True, text=True).stdout.split()
    return int(out[1]) if len(out) == 2 else -1


def main(build: str) -> int:
    daemon = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address"], stdout=subprocess.PIPE,
                              text=True)
    address = daemon.stdout.readline().strip()
    bus = Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION)
    near = [False]
    claims = []

    def sensor_call(_c, _s, _p, _i, method, _args, invocation):
        claims.append(method)
        invocation.return_value(None)

    def sensor_get(_c, _s, _p, _i, prop):
        return GLib.Variant("b", True if prop == "HasProximity" else near[0])

    def manager_call(_c, _s, _p, _i, _m, _a, invocation):
        invocation.return_value(GLib.Variant("(a{oa{sa{sv}}})", ({},)))

    bus.register_object("/net/hadess/SensorProxy", Gio.DBusNodeInfo.new_for_xml(SENSOR).interfaces[0],
                        sensor_call, sensor_get, None)
    bus.register_object("/org/freedesktop/ModemManager1", Gio.DBusNodeInfo.new_for_xml(MANAGER).interfaces[0],
                        manager_call, None, None)
    for name in ("net.hadess.SensorProxy", "org.freedesktop.ModemManager1"):
        bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", (name, 4)), None, 0, -1, None)

    def proximity(value: bool) -> None:
        near[0] = value
        bus.emit_signal(None, "/net/hadess/SensorProxy", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        GLib.Variant("(sa{sv}as)", ("net.hadess.SensorProxy", {"ProximityNear": GLib.Variant("b", value)}, [])))

    def settle() -> None:
        end = time.monotonic() + SETTLE_S
        while time.monotonic() < end:
            GLib.MainContext.default().iteration(False)
            time.sleep(0.05)

    def line(timeout_s: float) -> str:
        """The player's next line, serving the fake services meanwhile (it calls them synchronously)."""
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            GLib.MainContext.default().iteration(False)
            if select.select([player.stdout], [], [], 0.05)[0]:
                return player.stdout.readline().strip()
        return ""

    before = sink(), mode()
    player = subprocess.Popen([f"{build}/try-earpiece"], env=dict(os.environ, NEO_MESSAGING_SYSTEM_BUS=address),
                              stdout=subprocess.PIPE, text=True)
    ok = True

    def check(what: str, want_earpiece: bool) -> None:
        nonlocal ok
        settle()
        s, m = sink(), mode()
        good = ("Earpiece" in s) == want_earpiece and (m == 1) == want_earpiece
        ok &= good
        print(f"{'ok  ' if good else 'FAIL'} {what}: sink {s}, callaudiod mode {m}")

    try:
        assert line(SETTLE_S * 4) == "playing"
        check("playing, away from the ear", False)
        proximity(True)
        check("held to the ear", True)
        proximity(False)
        check("moved away", False)
        proximity(True)
        check("held to the ear again", True)
        bus.emit_signal(None, "/org/freedesktop/ModemManager1", "org.freedesktop.DBus.ObjectManager",
                        "InterfacesAdded", GLib.Variant("(oa{sa{sv}})", ("/org/freedesktop/ModemManager1/Call/1",
                                                                          {"org.freedesktop.ModemManager1.Call": {}})))
        check("a call comes", False)
        stopped = line(SETTLE_S) == "stopped"
        ok &= stopped
        print(f"{'ok  ' if stopped else 'FAIL'} the call stopped the note")
        # a call has the sound (callaudiod in call mode, as the dialer puts it): the ear changes nothing, and moving
        # away does not take call mode from the call
        proximity(False)
        settle()
        subprocess.run(CALL_AUDIO + ["SelectMode", "u", "1"], capture_output=True)
        proximity(True)
        settle()
        proximity(False)
        settle()
        kept = mode() == 1
        ok &= kept
        print(f"{'ok  ' if kept else 'FAIL'} in a call, the ear and away leave the call's mode alone")
        subprocess.run(CALL_AUDIO + ["SelectMode", "u", "0"], capture_output=True)
    finally:
        player.terminate()
        player.wait()
        settle()
        released = "ReleaseProximity" in claims
        ok &= released and (sink(), mode()) == before
        print(f"{'ok  ' if released else 'FAIL'} the sensor claimed {claims.count('ClaimProximity')}, released; "
              f"the sound as before: {(sink(), mode()) == before}")
        daemon.terminate()
    print("all passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
