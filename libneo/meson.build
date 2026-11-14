# libneo: the phone apps' shared widgets (Material 3, the way LineageOS draws it), on GTK 4 and libspongy.
# Its own project, like the apps that use it: build and install libspongy (the top-level project) first.
project('libneo', 'c', version: '0.1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

libm = meson.get_compiler('c').find_library('m')
gtk4 = dependency('gtk4', version: '>= 4.14')
spongy_dep = dependency('spongy-1')
spongy_gtk_dep = dependency('spongy-gtk-1')

gnome = import('gnome')
neo_resources = gnome.compile_resources('neo-resources', 'neo.gresource.xml', c_name: 'neo')

neo_headers = files(
  'neo.h',
  'neo-style.h',
  'neo-anim.h',
  'neo-ripple.h',
  'neo-pressable.h',
  'neo-switch.h',
  'neo-nav-bar.h',
  'neo-snackbar.h',
  'neo-dialog.h',
  'neo-time-picker.h',
)
neo_lib = shared_library('neo-1',
  'neo-style.c',
  'neo-anim.c',
  'neo-ripple.c',
  'neo-pressable.c',
  'neo-switch.c',
  'neo-nav-bar.c',
  'neo-snackbar.c',
  'neo-dialog.c',
  'neo-time-picker.c',
  neo_resources,
  dependencies: [gtk4, libm, spongy_dep, spongy_gtk_dep],
  version: '0.1.0',
  install: true)
neo_dep = declare_dependency(link_with: neo_lib, include_directories: include_directories('.'),
  dependencies: [gtk4, spongy_dep, spongy_gtk_dep])
install_headers(neo_headers, subdir: 'neo-1')
import('pkgconfig').generate(neo_lib, name: 'neo-1', subdirs: 'neo-1', requires: ['gtk4', 'spongy-1', 'spongy-gtk-1'],
  description: 'The phone apps\' Material 3 widgets')

# Roboto as Android ships it (variable: weight 100-900, width 75-100), under its own family name so that the
# system's static Roboto cut can never stand in for it
install_data('fonts/NeoRoboto.ttf', install_dir: get_option('datadir') / 'fonts' / 'neo')
