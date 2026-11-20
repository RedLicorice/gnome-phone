project('neo-voice', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare', language: 'c')

gnome = import('gnome')
gtk = dependency('gtk4', version: '>= 4.14')
cc = meson.get_compiler('c')
datadir = get_option('prefix') / get_option('datadir')
resources = gnome.compile_resources('neo-voice-resources', 'src/neo-voice.gresource.xml', source_dir: 'src')

executable('neo-voice',
  'src/nv-main.c', 'src/nv-window.c', 'src/nv-store.c', 'src/nv-wave.c', resources,
  install: true,
  dependencies: [gtk, dependency('gio-unix-2.0'), dependency('json-glib-1.0'), dependency('neo-1'),
                 cc.find_library('m')])

install_data('data/de.yesman.NeoVoice.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.NeoVoice.svg', install_dir: datadir / 'icons' / 'hicolor' / 'scalable' / 'apps')
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)
