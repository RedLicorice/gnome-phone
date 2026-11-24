project('neo-contacts', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="neo-contacts"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                      '-DCT_VERSION="@0@"'.format(meson.project_version()), language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
ebook = dependency('libebook-1.2')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# the Dialer's Roboto text boxes, phone number formatting and letter tiles, used as they are (read-only)
dialer = include_directories('../dialer/src')
src = include_directories('src')
# Neo Settings' door to `minerva-config` (ns-mc.c, GIO and json-glib only), for the Minerva parts (ct-mnv.c)
settings = include_directories('../settings/src')

# the contacts, their vCards and the list's rows: no GTK, so the tests and the seeder link them alone
json = dependency('json-glib-1.0')
core = static_library('contacts-core', 'src/ct-contact.c', 'src/ct-vcard.c', 'src/ct-store.c', 'src/ct-rows.c',
                      'src/ct-mnv-core.c', '../dialer/src/im-form.c', include_directories: dialer,
                      dependencies: [gio, ebook, m, json])

gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('contacts-resources', 'src/contacts.gresource.xml', source_dir: ['src', 'data'])
executable('neo-contacts', 'src/ct-main.c', 'src/ct-window.c', 'src/ct-page.c', 'src/ct-people.c', 'src/ct-list.c',
           'src/ct-quick.c', 'src/ct-editor.c', 'src/ct-picker.c', 'src/ct-settings.c', 'src/ct-dialogs.c',
           'src/ct-popup.c', 'src/ct-ui.c', 'src/ct-avatar.c', 'src/ct-minerva.c', 'src/ct-minerva-pages.c',
           'src/ct-mnv.c', 'src/ct-removal.c', '../dialer/src/text.c', '../dialer/src/phone-number.c', '../dialer/src/avatar.c',
           '../settings/src/ns-mc.c', resources,
           include_directories: [dialer, settings], link_with: core, install: true,
           dependencies: [gio, ebook, m, gtk, json, dependency('neo-1'),
                          dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/de.yesman.Contacts.service.in', output: 'de.yesman.Contacts.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Contacts.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.Contacts.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Contacts.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['ct-contact', 'ct-vcard', 'ct-rows', 'ct-mnv']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', include_directories: [src, dialer], link_with: core,
                        dependencies: [gio, ebook, m, json]))
endforeach

# fake contacts for the test rig's private address book (tests/try-contacts.sh); not installed
executable('ct-seed', 'tests/ct-seed.c', include_directories: [src, dialer], link_with: core,
           dependencies: [gio, ebook, m, dependency('gdk-pixbuf-2.0')])
