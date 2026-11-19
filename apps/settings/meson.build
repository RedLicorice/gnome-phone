project('neo-settings', 'c', version: '0.2', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
add_project_arguments('-DNS_VERSION="@0@"'.format(meson.project_version()), language: 'c')
# the keyring helper the pattern dialog runs, where this project installs it
add_project_arguments('-DNS_REKEY="@0@"'.format(get_option('prefix') / get_option('libexecdir') / 'neo-keyring-rekey'),
                      language: 'c')

gnome = import('gnome')
cc = meson.get_compiler('c')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('libadwaita-1', version: '>= 1.5'),
        dependency('json-glib-1.0'), dependency('goa-backend-1.0'), dependency('gio-unix-2.0'), dependency('gmodule-2.0'),
        dependency('libnm'), dependency('libdrm'), dependency('libpulse-mainloop-glib'),
        # the pattern dialog checks the current password the way the lock screen does
        cc.find_library('pam'), cc.find_library('m', required: false)]
# the lists scroll the way the launcher's drawer does (gnome-phone's libspongy), where it is installed
spongy = dependency('spongy-gtk-1', required: false)
if spongy.found()
  deps += spongy
  add_project_arguments('-DHAVE_SPONGY', language: 'c')
endif
resources = gnome.compile_resources('ns-resources', 'src/neo-settings.gresource.xml', source_dir: 'src')

executable('neo-settings',
  'src/main.c', 'src/ns-ui.c', 'src/ns-pages.c', 'src/ns-parallax.c', 'src/ns-net.c', 'src/ns-bt.c',
  'src/ns-sound.c', 'src/ns-apps.c', 'src/ns-shell.c', 'src/ns-fprint.c', 'src/ns-pattern.c', 'src/ns-privacy.c', 'src/ns-system.c', 'src/ns-look.c',
  'src/ns-minerva.c', 'src/ns-prompt.c', 'src/ns-places.c', 'src/ns-events.c', 'src/ns-provider.c', 'src/ns-mc.c', 'src/ns-charge.c', 'src/ns-apikeys.c', 'src/ns-account.c', 'src/ns-ssh.c', 'src/ns-perm.c', 'src/ns-agents.c',
  resources,
  dependencies: deps,
  install: true)
install_data('data/de.yesman.NeoSettings.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/de.yesman.NeoSettings.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('data/de.yesman.NeoSettings.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
install_data('data/neo-keyring-rekey', install_dir: get_option('libexecdir'), install_mode: 'rwxr-xr-x')
# D-Bus activation (DBusActivatable=true): whatever opens a page through GApplication's "launch-panel" action,
# GNOME's way of opening a Settings panel, starts the app when it is not running
service = configuration_data()
service.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(input: 'data/de.yesman.NeoSettings.service.in', output: 'de.yesman.NeoSettings.service',
               configuration: service, install_dir: get_option('datadir') / 'dbus-1' / 'services')
gnome.post_install(glib_compile_schemas: true)
