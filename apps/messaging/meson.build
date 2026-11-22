project('neo-messaging', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="neo-messaging"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

gio = dependency('gio-unix-2.0', version: '>= 2.76')
ebook = dependency('libebook-1.2')
sqlite = dependency('sqlite3')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# the Phone's numbers, contacts, notifications and app opening are the Messaging's too (Android's Messaging and
# Dialer share them through the platform): built from its sources, not copied
dialer = include_directories('../dialer/src')
store = static_library('store', 'src/sms-store.c', dependencies: [gio, sqlite])
# the configuration layers (Minerva's docs/packaging.md, "Configuration"): shipped defaults, the device's, /etc, the
# owner's changed keys; the reader is Minerva's own (common/minerva-layers.c, vendored)
layers = static_library('layers', '../../common/minerva-layers.c', dependencies: [gio],
                        c_args: ['-DMINERVA_LAYERS_FILE="@0@"'.format(get_option('layers-file'))])
provider = static_library('provider', 'src/sms-provider.c', link_with: layers,
                          include_directories: include_directories('../../common'), dependencies: [gio])
phone = static_library('phone', '../dialer/src/phone-number.c', '../dialer/src/contacts.c', '../dialer/src/phone.c',
                       '../dialer/src/notifier.c', '../dialer/src/blocked-numbers.c', include_directories: dialer,
                       dependencies: [gio, ebook, sqlite],
                       c_args: ['-DNOTIFIER_APP_ID="de.yesman.Messaging"', '-DNOTIFIER_APP_NAME="Messaging"'])

# the service: the messages with the app closed, taking them off the modem, sending, the notifications
md4c = dependency('md4c')
executable('neo-messaging-sms', 'src/sms-service.c', 'src/sms-modem.c', 'src/msg-markdown.c',
           'src/msg-callers.c', c_args: ['-DCALLERS_FILE="@0@"'.format(get_option('callers-file'))],
           link_with: [store, phone, provider], include_directories: dialer, dependencies: [gio, ebook, md4c], install: true)

# the app: the conversation list, a conversation, the contact picker
gnome = import('gnome')
gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('messaging-resources', 'src/messaging.gresource.xml', source_dir: ['src', 'data'])
executable('neo-messaging', 'src/messaging-main.c', 'src/messaging-window.c', 'src/msg-navigator.c',
           'src/blocked-contacts.c', 'src/people-options.c', 'src/msg-settings.c', 'src/msg-directory.c', 'src/msg-sound.c',
           'src/conversation-list.c', 'src/conversation-view.c', 'src/contact-picker.c', 'src/msg-action-bar.c',
           'src/msg-add-contact.c', 'src/msg-avatar.c', 'src/msg-bubble.c', 'src/msg-dates.c', 'src/msg-image.c',
           'src/msg-markdown.c', 'src/msg-media.c', 'src/msg-voice.c', 'src/msg-agent-output.c', 'src/msg-people.c', 'src/msg-assistant.c', 'src/msg-swipe.c', 'src/sms-client.c',
           'src/sms-segments.c',
           '../dialer/src/popup-menu.c', '../dialer/src/contacts-app.c', '../dialer/src/app-action.c', resources,
           link_with: [store, phone, provider],
           include_directories: [dialer, include_directories('../../common')], install: true,
           c_args: ['-DSOUNDS_DIR="@0@"'.format(datadir / 'neo-messaging/sounds'),
                    '-DASSISTANT_FILE="@0@"'.format(get_option('assistant-file'))],
           dependencies: [gio, ebook, gtk, dependency('json-glib-1.0'), dependency('neo-1'), dependency('spongy-gtk-1'),
                          dependency('gstreamer-1.0'), dependency('glycin-gtk4-2'), md4c,
                          meson.get_compiler('c').find_library('m')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/neo-messaging-sms.service.in', output: 'neo-messaging-sms.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/de.yesman.Messaging.Sms.service.in', output: 'de.yesman.Messaging.Sms.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/de.yesman.Messaging.service.in', output: 'de.yesman.Messaging.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Messaging.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.Messaging.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Messaging.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
install_data('data/sounds/message_sent.wav', install_dir: datadir / 'neo-messaging/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

test('sms-store', executable('test-sms-store', 'tests/test-sms-store.c', link_with: store, dependencies: [gio]))
test('sms-segments', executable('test-sms-segments', 'tests/test-sms-segments.c', 'src/sms-segments.c',
                                dependencies: [gio]))
test('msg-markdown', executable('test-msg-markdown', 'tests/test-msg-markdown.c', 'src/msg-markdown.c',
                                 include_directories: 'src', dependencies: [gio, md4c]))
test('sms-group', executable('test-sms-group', 'tests/test-sms-group.c', link_with: provider, dependencies: [gio]))
test('sms-layers', executable('test-sms-layers', 'tests/test-sms-layers.c', link_with: provider, dependencies: [gio]))
# the message flows end to end, against a mock modem and notification server on buses of their own
test('sms-flow', find_program('python3'), timeout: 120,
     args: [files('tests/test-sms.py'), meson.current_build_dir(), meson.current_source_dir()])
