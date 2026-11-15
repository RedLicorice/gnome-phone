project('neo-prompt', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare', language: 'c')

gnome = import('gnome')
gtk = dependency('gtk4', version: '>= 4.14')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
resources = gnome.compile_resources('neo-prompt-resources', 'src/neo-prompt.gresource.xml', source_dir: 'src')

executable('neo-prompt',
  'src/np-main.c', 'src/np-window.c', 'src/np-store.c', 'src/np-editor.c', 'src/np-context.c',
  'src/np-history.c', 'src/np-diff.c', 'src/np-ui.c', resources,
  install: true,
  dependencies: [gtk, dependency('json-glib-1.0'), dependency('neo-1')])

# D-Bus activation (DBusActivatable=true): Settings' editor rows and the Contacts card open a document through
# GApplication's Open even when the app is not running
service = configuration_data()
service.set('bindir', bindir)
configure_file(input: 'data/de.yesman.NeoPrompt.service.in', output: 'de.yesman.NeoPrompt.service',
               configuration: service, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.NeoPrompt.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.NeoPrompt.svg', install_dir: datadir / 'icons' / 'hicolor' / 'scalable' / 'apps')
gnome.post_install (gtk_update_icon_cache: true, update_desktop_database: true)
