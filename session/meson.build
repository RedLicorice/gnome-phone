# The session's system files, installed by a package (debian/: gnome-phone-session, gnome-phone-daemons, sim-apn)
# Its own project, like libneo and the apps; the programs the units start come from the
# top-level project (input-boost, neo-wakeup, gp-auth) or from here (neo-dock, sim-apn).
#
# Left out on purpose: environment.d/60-spongy.conf (one phone's density) and org.gnome.Shell@.service.d/50-mobile.conf
# (a shell built into /opt by hand).
project('gnome-phone-session', version: '0.1.0', meson_version: '>= 1.0')

prefix = get_option('prefix')
sbindir = prefix / get_option('sbindir')
libexecdir = prefix / get_option('libexecdir')
datadir = get_option('datadir')
systemd_system = prefix / 'lib' / 'systemd' / 'system'
systemd_user = prefix / 'lib' / 'systemd' / 'user'

# the units name the hand-installed programs in /usr/local
local_paths = ['sed', '-e', 's|/usr/local/sbin/|' + sbindir + '/|g', '-e', 's|/usr/local/libexec/|' + libexecdir + '/|g',
               '@INPUT@']
foreach unit : ['gnome-phone.service', 'gnome-desktop.service', 'neo-dock.service', 'input-boost.service',
                'neo-wakeup.service', 'sim-apn.service']
  configure_file(input: unit, output: unit, command: local_paths, capture: true, install_dir: systemd_system)
endforeach

install_data('neo-dock', 'sim-apn', install_dir: sbindir, install_mode: 'rwxr-xr-x')
# the dock switcher's udev rule is opt-in: on a plug or an unplug neo-dock starts a session, which would take over a
# desktop that did not ask for it. Linked into /etc/udev/rules.d by whoever wants it
install_data('71-neo-dock.rules', install_dir: datadir / 'gnome-phone-session')
# the lock screen's PAM service (gp-auth)
install_data('pam.d/gnome-phone', 'pam.d/gnome-phone-fingerprint', 'pam.d/gnome-phone-voice',
             install_dir: get_option('sysconfdir') / 'pam.d')
# the voice step of the lock screen's PAM service: asks the Minerva voice service for a live check
install_data('gp-voice-check', install_dir: libexecdir, install_mode: 'rwxr-xr-x')
# neo-wakeup's bus name
install_data('de.yesman.Wakeup.conf', install_dir: datadir / 'dbus-1' / 'system.d')

# GNOME Phone as a session a display manager offers (gdm/): GNOME's own session with the shell in the gnome-phone
# mode, whose only difference is the extension, always on
install_data('gdm/gnome-phone.json', install_dir: datadir / 'gnome-shell' / 'modes')
install_data('gdm/gnome-phone.session', install_dir: datadir / 'gnome-session' / 'sessions')
install_data('gdm/gnome-phone.session.conf', install_dir: systemd_user / 'gnome-session@gnome-phone.target.d')
install_data('gdm/gnome-phone.desktop', install_dir: datadir / 'wayland-sessions')
# GNOME's own screen lock off: the keyguard is the lock screen (see the file)
install_data('90_gnome-phone.gschema.override', install_dir: datadir / 'glib-2.0' / 'schemas')
