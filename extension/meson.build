# The gnome-phone GNOME Shell extension, installed system-wide (debian/: gnome-shell-extension-gnome-phone).
# Its own project, like libneo and the apps. Everything in this directory is the extension except the generators in
# tools/ (vd2js.py, l3icons.py: they read an Android source tree; hk2json.py, dict2bin.py: the touch keyboard's
# layouts and dictionaries), the tests in tests/ and Python's caches.
project('gnome-shell-extension-gnome-phone', version: '0.1.0', meson_version: '>= 1.0')

uuid = 'gnome-phone@yesman.de'
extdir = get_option('datadir') / 'gnome-shell' / 'extensions' / uuid
bindir = get_option('prefix') / get_option('bindir')

install_subdir('.', install_dir: extdir, strip_directory: true,
  exclude_directories: ['tools', 'tests', '__pycache__'],
  exclude_files: ['meson.build', 'home.js', 'appmem.py'])

# the touch keyboard's word suggestions on the shipped dictionaries (plain gjs, no shell needed)
gjs = find_program('gjs', required: false)
if gjs.found()
  test('predictor', gjs, args: ['-m', files('tests/predictor.test.js')])
  test('camera-indicator', gjs, args: ['-m', files('tests/camera.test.js')])
  test('internet', gjs, args: ['-m', files('tests/internet.test.js')])
  # the look: effective look over all settings, Color changed in place, the wallpaper and icon pack swaps (needs mutter's
  # Cogl typelib and glib-compile-schemas; the script skips otherwise)
  test('theme', find_program('bash'), args: [files('tests/theme-test.sh')])
  # the lock screen's fingerprint states and words: plain data, no shell library to import
  test('finger', gjs, args: ['-m', files('tests/finger.test.js')])
endif

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
