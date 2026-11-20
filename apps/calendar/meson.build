project('neo-calendar', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
ecal = dependency('libecal-2.0', version: '>= 3.46')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
gettext_args = ['-DGETTEXT_PACKAGE="neo-calendar"',
                '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                '-DCAL_VERSION="@0@"'.format(meson.project_version())]

# the events and their rules (cal-model), their text (cal-format) and evolution-data-server (cal-store): shared by
# the app, the reminder service and the tests
json = dependency('json-glib-1.0')
# Neo Settings' door to `minerva-config` (ns-mc.c, GIO and json-glib only), for the Minerva calendar (cal-minerva.c)
settings = include_directories('../settings/src')
core = static_library('calcore', 'src/cal-model.c', 'src/cal-format.c', 'src/cal-store.c', 'src/cal-alerts.c',
                      'src/cal-minerva.c', '../settings/src/ns-mc.c', include_directories: settings,
                      c_args: gettext_args, dependencies: [gio, ecal, m, json])

# the app
resources = gnome.compile_resources('calendar-resources', 'src/calendar.gresource.xml', source_dir: ['src', 'data'])
executable('neo-calendar', 'src/cal-main.c', 'src/cal-window.c', 'src/cal-text.c', 'src/day-view.c',
           'src/month-view.c', 'src/agenda-view.c', 'src/cal-popup.c', 'src/cal-dialogs.c', 'src/cal-page.c',
           'src/cal-date-picker.c', 'src/cal-reminders.c', 'src/event-info.c', 'src/event-editor.c',
           'src/cal-settings.c', 'src/cal-compat-switch.c', 'src/cal-time-picker.c', 'src/cal-colors.c',
           'src/cal-prefs.c', 'src/cal-tz-picker.c', 'src/cal-search.c', 'src/cal-accounts.c',
           'src/cal-color-picker.c', 'src/cal-material-switch.c', 'src/cal-recurrence-picker.c',
           'src/cal-alert-page.c', 'src/cal-outlined-field.c', resources,
           link_with: core, install: true, c_args: gettext_args, include_directories: settings,
           dependencies: [gio, ecal, m, json, dependency('gtk4', version: '>= 4.14'), dependency('neo-1'),
                          dependency('spongy-gtk-1'), dependency('icu-i18n'), dependency('icu-uc')])

# the reminder service: notifies at each reminder, and wakes the phone for it, with the app closed
executable('neo-calendar-reminders', 'src/reminders-main.c', link_with: core, install: true, c_args: gettext_args,
           dependencies: [gio, ecal, m, json])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/neo-calendar-reminders.service.in', output: 'neo-calendar-reminders.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/de.yesman.Calendar.Reminders.service.in', output: 'de.yesman.Calendar.Reminders.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/de.yesman.Calendar.service.in', output: 'de.yesman.Calendar.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Calendar.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.Calendar.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Calendar.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

# the test rig's events (tests/try-calendar.sh): refuses to run outside the rig
executable('cal-seed', 'tests/cal-seed.c', dependencies: [gio, ecal])

foreach name : ['cal-model', 'cal-format', 'cal-alerts', 'cal-minerva']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', link_with: core, c_args: gettext_args,
                        dependencies: [gio, ecal, m, json]))
endforeach
# the repeat dialog's rules: cal-recurrence-picker.c without its dialog
test('cal-recurrence', executable('test-cal-recurrence', 'tests/test-cal-recurrence.c', 'src/cal-recurrence-picker.c',
                                  c_args: gettext_args + ['-DCAL_RECURRENCE_NO_UI'],
                                  dependencies: [gio, m, dependency('gtk4')]))
