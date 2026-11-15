# GNOME Phone

GNOME Phone makes GNOME Shell feel like an Android phone. It adds the Neo launcher as the home screen, Android's
gesture navigation (swipe up for home and recents, swipe in from an edge for back), the status bar, the notification
shade, the lock screen and the always-on display, plus a set of phone apps written in GTK 4. It is a GNOME Shell
extension and a session, not a shell of its own, and it does not use Phosh.

**Which GNOME Shell.** The phones run GNOME Shell's mobile branch: gnome-shell-mobile and mutter-mobile, branch
mobile-51 (GNOME Shell 51.1.mobile.22), built into `/opt/gnome-mobile`. Debian's own gnome-shell 51.0 loads the
extension, but on a phone it crashes (SIGSEGV) right after the lock screen is unlocked. These packages do not
include the mobile shell; the POCO X3 NFC ROM zip does.

It was developed as the everyday shell of a Samsung Galaxy S10e running mainline Linux and Mobian, and is being
brought up on a POCO X3 NFC.

## What's in it

| Package | What it is |
|---|---|
| `gnome-phone` | Metapackage: the extension, the session and the apps |
| `gnome-shell-extension-gnome-phone` | The GNOME Shell extension: gestures, recents, status bar, shade, lock screen, AOD, rotation |
| `gnome-phone-session` | The "GNOME Phone" session (GDM, or a systemd unit with no display manager) |
| `gnome-phone-daemons` | System helpers: the lock screen's PIN check (PAM), waking from suspend for alarms and reminders, a GPU clock boost while touched |
| `neo-launcher` | Neo, the home screen: pages, dock, app drawer, widgets, icon packs and shapes, parallax wallpaper (a Wayland client drawn with Vulkan) |
| `neo-dialer`, `neo-messaging`, `neo-contacts` | LineageOS's Dialer and Messaging rebuilt in GTK 4 (calls and SMS through ModemManager, ringing with the app closed); contacts on evolution-data-server |
| `neo-clock`, `neo-calendar`, `neo-calculator`, `neo-compass` | Alarms, timers and world clock; calendar; calculator; compass |
| `neo-browser` | A browser with Chrome for Android's phone UI (tab grid, bottom toolbar, incognito) on WebKitGTK 6 |
| `neo-settings` | The Settings app: connections, sounds, notifications, display, battery, wallpaper and parallax, the home screen, the lock screen, location, accounts, apps, language, accessibility. GNOME Phone opens it wherever GNOME opens a Settings panel |
| `neo-weather` | The weather where the phone is (a city chosen by name, which works with Location off; else GNOME Weather's first city; else GeoClue), from Open-Meteo |
| `neo-camera` | LineageOS's Aperture rebuilt in GTK 4 on libcamera and GStreamer: photos, videos, barcode scanning |
| `neo-files` | File Manager+ rebuilt in GTK 4 on GIO and GVfs: storage, removable media, recycle bin, SMB/SFTP/FTP/WebDAV |
| `neo-iconpack-round` | Round, the default icon pack: white Material Symbols glyphs (Apache-2.0) on gradient discs |
| `neo-parallax-solar-system` | The default wallpaper: a tilt-parallax solar system from NASA's public-domain images |
| `sim-apn` | Sets up a mobile data profile for every new SIM |
| `libneo-1-0`, `libspongy-1-0`, `libspongy-gtk-1-0`, `gir1.2-spongy-1`, `fonts-neo-roboto` | The apps' Material 3 widgets, Android-style scroll physics and Roboto |

The release also carries **mutter 51.0-3+gp1**: Debian's mutter 51.0-3 with the five patches in
[`patches/mutter`](patches/mutter). A phone needs two of them: one keeps the panel powered for the always-on
display, the other lets a 1080x2280 panel use scale 3. Without them GNOME Phone still runs, but with no always-on
display and at scale 2.5. All five patches do nothing on a desktop.

## Requirements

- Debian unstable (sid) or Mobian staging, arm64, with GNOME Session 51 and GNOME Shell 51. On a phone that means
  the mobile branch in `/opt/gnome-mobile` (see above), started by a drop-in for the session's shell unit,
  `/etc/systemd/user/org.gnome.Shell@gnome-phone.service.d/50-mobile.conf`:

  ```ini
  [Service]
  ExecStart=
  ExecStart=/opt/gnome-mobile/bin/gnome-shell --mode=%i
  Environment=GSETTINGS_SCHEMA_DIR=/opt/gnome-mobile/share/glib-2.0/schemas
  ```
- A phone with a working mainline kernel: display, touch, and ModemManager for calls and SMS
- Vulkan for the launcher (Mesa: Panfrost/panvk on Mali, Turnip on Adreno; lavapipe works, slowly)

## Install

Download the `.deb` files from the [latest release](https://github.com/pulpoff/gnome-phone/releases/latest) into an
empty folder, then:

```sh
sudo apt install ./*.deb
sudo apt-mark hold libmutter-51-0 gir1.2-mutter-51 mutter-common mutter-common-bin
```

The hold keeps the patched mutter from being replaced by Debian's at the next upgrade. Skip the mutter `.deb` files
and the hold if you want Debian's stock mutter.

Then choose **GNOME Phone** in the session menu at the GDM login screen. To make it the default for automatic login:

```sh
busctl call org.freedesktop.Accounts /org/freedesktop/Accounts/User$(id -u) \
    org.freedesktop.Accounts.User SetSession s gnome-phone
```

To run without a display manager, see `/usr/share/doc/gnome-phone-session/README.Debian`.

## Not in these packages

- **Device support.** Kernel, firmware, modem and sensor setup, the camera hole's position, and audio routing belong
  in a per-device package. The camera hole is read from libgmobile's display-panel data, or from
  `/etc/gmobile/display-panels/<compatible>.json`.
- **The mobile GNOME Shell** (see the top of this page). The POCO X3 NFC ROM zip carries it in `/opt/gnome-mobile`.
- **Other icon packs and parallax wallpaper sets.** Only Round and the NASA solar system are here. Neo finds icon
  packs in `neo-launcher/iconpacks` and parallax sets in `neo-launcher/parallax`, in `~/.local/share` or any
  system data directory (`/usr/share`), and Neo Settings lists both.
- **Android's ringtones.** The dialer falls back to the freedesktop sound theme's ringtone.

## Build from source

```sh
sudo apt build-dep .
dpkg-buildpackage -b -uc -us
```

Each part (`libneo`, `libspongy`, `extension`, `session`, `neo-launcher`, `apps/*`) is its own meson project;
`debian/rules` builds them in order.

## License

GNOME Phone's own code is under the [MIT license](LICENSE). Icons, images and the world-clock table converted from
AOSP and LineageOS are Apache-2.0, as are the Material Symbols glyphs of the Round icon pack. The solar-system
wallpaper's planets, Moon and Sun are NASA's public-domain images; its README credits each one. The calculator's
constructive reals are under the SGI/HP CRCalc notice; Roboto is OFL-1.1. [`debian/copyright`](debian/copyright)
lists every file. The mutter patches are GPL-2.0-or-later, like mutter.
