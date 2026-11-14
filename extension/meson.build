# The gnome-phone GNOME Shell extension, installed system-wide (debian/: gnome-shell-extension-gnome-phone).
# Its own project, like libneo and the apps. Everything in this directory is the extension except the generators in
# tools/ (vd2js.py, l3icons.py: they read an Android source tree) and Python's caches.
project('gnome-shell-extension-gnome-phone', version: '0.1.0', meson_version: '>= 1.0')

uuid = 'gnome-phone@yesman.de'
extdir = get_option('datadir') / 'gnome-shell' / 'extensions' / uuid
bindir = get_option('prefix') / get_option('bindir')

install_subdir('.', install_dir: extdir, strip_directory: true,
  exclude_directories: ['tools', '__pycache__'],
  exclude_files: ['meson.build', 'home.js', 'appmem.py'])

# appmem.py runs as a program of its own (recents.js)
install_data('appmem.py', install_dir: extdir, install_mode: 'rwxr-xr-x')

# the touch keyboard's settings (Neo Settings has a page for them)
install_data('schemas/de.yesman.GnomePhone.Keyboard.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')

# home.js starts the launcher by its path; a hand-installed launcher lives in /usr/local, the packaged one in bindir
configure_file(input: 'home.js', output: 'home.js', install_dir: extdir,
  command: ['sed', '-e', 's|/usr/local/bin/neo-launcher|' + bindir + '/neo-launcher|', '@INPUT@'], capture: true)

# the optional look (libneo's apps and Neo Settings read it too)
install_data('schemas/de.yesman.GnomePhone.Theme.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
