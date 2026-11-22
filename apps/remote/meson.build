project('neo-remote', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare', language: 'c')

gnome = import('gnome')
datadir = get_option('prefix') / get_option('datadir')
resources = gnome.compile_resources('neo-remote-resources', 'src/neo-remote.gresource.xml', source_dir: 'src')

executable('neo-remote', 'src/nr-main.c', 'src/nr-ir.c', 'src/nr-window.c', resources,
  install: true,
  dependencies: [dependency('gtk4', version: '>= 4.14'), dependency('json-glib-1.0'), dependency('neo-1')])

install_data('data/de.yesman.NeoRemote.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.NeoRemote.svg', install_dir: datadir / 'icons' / 'hicolor' / 'scalable' / 'apps')
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)
