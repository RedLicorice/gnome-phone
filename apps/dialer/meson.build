project('neo-dialer', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="neo-dialer"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
ebook = dependency('libebook-1.2')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# numbers, the call log and the contacts: shared by the service, the app and the tests
json = dependency('json-glib-1.0')
common = static_library('common', 'src/phone-number.c', 'src/call-log.c', 'src/contacts.c', 'src/im-form.c',
                        'src/blocked-numbers.c', dependencies: [gio, ebook, json, dependency('sqlite3')])

# the service: the calls with the app closed, ringing, the call log, the notifications
executable('neo-dialer-calls', 'src/calls-service.c', 'src/calls.c', 'src/modem.c', 'src/ringer.c',
           'src/call-notifications.c', 'src/notifier.c', 'src/phone.c', link_with: common, install: true,
           dependencies: [gio, ebook, m, dependency('gstreamer-1.0'), dependency('libpulse-mainloop-glib')])

# the app: its tabs and dialpad, and the in-call screen (neo-dialer --incall)
gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('dialer-resources', 'src/dialer.gresource.xml', source_dir: ['src', 'data'])
executable('neo-dialer', 'src/dialer-main.c', 'src/dialer-window.c', 'src/dialpad-sheet.c', 'src/dialpad.c',
           'src/search-bar.c', 'src/search-page.c', 'src/nav-bar.c', 'src/favorites-page.c', 'src/recents-page.c',
           'src/contacts-page.c', 'src/contacts-app.c', 'src/app-action.c', 'src/call-history.c',
           'src/call-details.c', 'src/call-stats.c', 'src/dialer-settings.c', 'src/popup-menu.c', 'src/empty-view.c',
           'src/call-dates.c', 'src/text.c', 'src/incall-window.c', 'src/incall-view.c', 'src/incall-button.c',
           'src/answer-view.c', 'src/avatar.c', 'src/caller.c', 'src/calls-client.c', 'src/phone.c', resources,
           link_with: common, install: true,
           dependencies: [gio, ebook, m, gtk, dependency('neo-1'), dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/neo-dialer-calls.service.in', output: 'neo-dialer-calls.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/de.yesman.Dialer.Calls.service.in', output: 'de.yesman.Dialer.Calls.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/de.yesman.Dialer.service.in', output: 'de.yesman.Dialer.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Dialer.desktop', 'data/de.yesman.Dialer.InCall.desktop',
             install_dir: datadir / 'applications')
install_data('data/de.yesman.Dialer.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Dialer.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['call-log', 'phone-number', 'blocked-numbers', 'im-form']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', link_with: common,
                        dependencies: [gio, ebook, json, dependency('sqlite3')]))
endforeach

# the call flows end to end, against a mock modem, shell and notification server on buses of their own
test('calls-flow', find_program('python3'), timeout: 90,
     args: [files('tests/test-calls.py'), meson.current_build_dir(), meson.current_source_dir()])
