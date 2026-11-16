project('neo-camera', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

prefix = get_option('prefix')
datadir = prefix / get_option('datadir')
bindir = prefix / get_option('bindir')
pkgdatadir = datadir / 'neo-camera'

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare',
                      '-DGETTEXT_PACKAGE="neo-camera"',
                      '-DLOCALEDIR="@0@"'.format(prefix / get_option('localedir')),
                      '-DDATADIR="@0@"'.format(pkgdatadir), language: 'c')

gnome = import('gnome')
m = meson.get_compiler('c').find_library('m')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('gio-unix-2.0'), dependency('gdk-pixbuf-2.0'),
        dependency('gstreamer-1.0', version: '>= 1.22'), dependency('gstreamer-app-1.0'),
        dependency('gstreamer-video-1.0'), dependency('gstreamer-tag-1.0'), dependency('pangocairo'), dependency('json-glib-1.0'),
        dependency('neo-1'), dependency('spongy-gtk-1'), m]

resources = gnome.compile_resources('camera-resources', 'src/camera.gresource.xml', source_dir: ['src', 'data'])
executable('neo-camera',
           'src/cam-main.c', 'src/cam-window.c', 'src/cam-devices.c', 'src/cam-encoder.c', 'src/cam-pipeline.c',
           'src/cam-sensors.c', 'src/cam-recorder.c', 'src/cam-photo.c', 'src/cam-storage.c', 'src/cam-sounds.c',
           'src/cam-widgets.c', 'src/cam-mode-selector.c', 'src/cam-viewfinder.c', 'src/cam-qr-sheet.c',
           'src/cam-settings.c', 'src/cam-dialogs.c', 'src/cam-key-import.c', 'src/cam-tune.c', 'src/cam-tone.c',
           'src/cam-restart.c',
           resources,
           dependencies: deps, install: true)

configure_file(input: 'data/de.yesman.Camera.service.in', output: 'de.yesman.Camera.service',
               configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Camera.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.Camera.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Camera.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
# Aperture's camera sounds (AOSP frameworks/base data/sounds, Apache-2.0) and Droid Sans Mono (Apache-2.0)
install_data('data/sounds/camera_click.ogg', 'data/sounds/VideoRecord.ogg', 'data/sounds/VideoStop.ogg',
             install_dir: pkgdatadir / 'sounds')
install_data('data/fonts/DroidSansMono.ttf', install_dir: pkgdatadir / 'fonts')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

# the QR classifier, without GTK's display
test('qr-classify', executable('test-qr', 'tests/test-qr.c', 'src/cam-qr-sheet.c', 'src/cam-dialogs.c',
                               'src/cam-key-import.c', include_directories: 'src', dependencies: deps))

# the per-camera tuning storage, on GSettings' memory backend (compiles the schema itself)
test('tone', executable('test-tone', 'tests/test-tone.c', 'src/cam-tone.c', include_directories: 'src',
                        dependencies: deps, build_by_default: false),
     env: {'CAM_SCHEMA_DIR': meson.current_source_dir() / 'data'})

# a restarted Camera is a plain launch (service-mode argv left it without a window)
test('restart', executable('test-restart', 'tests/test-restart.c', 'src/cam-restart.c', include_directories: 'src',
                           dependencies: deps))
