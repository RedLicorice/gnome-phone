#!/bin/sh
# Installs the session files on the phone (as root): the session unit, the shell's drop-in, the input boost (its
# binary from the build: /usr/local/sbin/input-boost), the lock screen's PAM service (for /usr/local/libexec/gp-auth),
# the session's environment (libspongy's density) and the dock switcher (neo-dock, its unit and udev rule, and the
# docked desktop's session unit), sim-apn (a mobile data profile for every new SIM) and neo-wakeup (wakes the phone
# for alarm clocks; its binary from the build: /usr/local/sbin/neo-wakeup).
# GNOME Phone becomes the session the phone boots into.
set -e
cd "$(dirname "$0")"
install -m 644 gnome-phone.service gnome-desktop.service neo-dock.service input-boost.service sim-apn.service \
    neo-wakeup.service /etc/systemd/system/
install -m 755 neo-dock sim-apn /usr/local/sbin/
install -m 644 71-neo-dock.rules /etc/udev/rules.d/
install -m 644 pam.d/gnome-phone pam.d/gnome-phone-fingerprint pam.d/gnome-phone-voice /etc/pam.d/
install -m 755 gp-voice-check /usr/local/libexec/
install -m 644 environment.d/60-spongy.conf /etc/environment.d/
install -m 644 de.yesman.Wakeup.conf /etc/dbus-1/system.d/
install -d /etc/systemd/user/org.gnome.Shell@.service.d
install -m 644 org.gnome.Shell@.service.d/50-mobile.conf /etc/systemd/user/org.gnome.Shell@.service.d/
systemctl reload dbus
systemctl daemon-reload
systemctl enable input-boost.service gnome-phone.service neo-dock.service sim-apn.service neo-wakeup.service
