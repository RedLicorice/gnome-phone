deps = [
  dependency('vulkan'),
  dependency('wayland-client'),
  dependency('glib-2.0'),
  dependency('gio-unix-2.0'),
  dependency('gdk-pixbuf-2.0'),
  dependency('librsvg-2.0'),
  dependency('cairo'),
  dependency('freetype2'),
  dependency('harfbuzz'),
  dependency('fontconfig'),
  dependency('json-glib-1.0'),
  dependency('xkbcommon'),
  dependency('threads'),
  libm,
  spongy_dep,
]

# Wayland protocols: xdg-shell for the window, viewporter + fractional scale for the exact pixel size, presentation
# time for the frame clock, text input for the drawer's search (the on-screen keyboard)
scanner = find_program('wayland-scanner')
wp = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
protocols = [
  wp / 'stable/xdg-shell/xdg-shell.xml',
  wp / 'stable/viewporter/viewporter.xml',
  wp / 'stable/presentation-time/presentation-time.xml',
  wp / 'staging/fractional-scale/fractional-scale-v1.xml',
  wp / 'unstable/text-input/text-input-unstable-v3.xml',
]
proto_src = []
foreach p : protocols
  name = p.split('/')[-1].replace('.xml', '')
  proto_src += custom_target(name + '-code', input: p, output: name + '-protocol.c',
                             command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  proto_src += custom_target(name + '-header', input: p, output: name + '-client-protocol.h',
                             command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach

executable('neo-launcher',
  'src/main.c', 'src/vk.c', 'src/gfx.c', 'src/wl.c', 'src/text.c', 'src/image.c', 'src/parallax.c', 'src/chisel.c', 'src/apps.c',
  'src/icons.c', 'src/model.c', 'src/tiles.c', 'src/home.c', 'src/drawer.c', 'src/nav.c',
  'src/popup.c', 'src/drag.c',
  proto_src,
  include_directories: include_directories('src', 'shaders'),
  dependencies: deps,
  install: true)

# the launcher's settings (de.yesman.neo), which the gnome-phone extension and Neo Settings read too
install_data('data/de.yesman.neo.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
# the first start's home screen (model.c); a device or distribution can put its own in /etc/xdg/neo-launcher
install_data('data/layout.json', install_dir: get_option('datadir') / 'neo-launcher')
# the Round icon pack (tools/iconpack-round makes it), packaged apart as neo-iconpack-round with its default
install_subdir('iconpacks/round', install_dir: get_option('datadir') / 'neo-launcher' / 'iconpacks')
install_data('iconpacks/95_neo-iconpack-round.gschema.override',
             install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
# the NASA solar-system parallax set (tools/parallax-solar-system makes it), packaged apart as neo-parallax-solar-system
install_subdir('parallax/solar-system', install_dir: get_option('datadir') / 'neo-launcher' / 'parallax')
# the theme-v4 wallpapers (tools/parallax-renaissance/build_chisel.py makes them); the theme picks one by its look
foreach look : ['marble', 'sea', 'forest', 'plum']
  install_subdir('parallax/renaissance-' + look, install_dir: get_option('datadir') / 'neo-launcher' / 'parallax')
endforeach
install_data('parallax/95_neo-parallax-solar-system.gschema.override',
             install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
install_data('parallax/neo-parallax-solar-system.xml', install_dir: get_option('datadir') / 'gnome-background-properties')
gnome = import('gnome')
gnome.post_install(glib_compile_schemas: true)
