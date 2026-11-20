#!/bin/bash
# Deploy extension/ to a running phone over ssh, keep a backup of what it replaces, restart the session and report.
# Settings come from .env at the repository root (see .env.example) or the environment; the phone login comes from Minerva's .env (tools/phone).
#   usage: tools/deploy-extension.sh [--no-restart]
set -euo pipefail

# The phone side is Minerva's tools/phone (this tree is its submodule): ssh.sh logs in as the desktop user (key), ssh-root.sh
# as the operator for the root steps, shell.sh gives the active shell's data (shells.conf). PHONE_TOOLS_DIR overrides it.
root=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$root/.env" ] && { set -a; . "$root/.env"; set +a; }
tools=${PHONE_TOOLS_DIR:-$root/../tools/phone}
[ -x "$tools/ssh-root.sh" ] || { echo "no $tools/ssh-root.sh: this script needs Minerva's tools/phone (PHONE_TOOLS_DIR)" >&2; exit 1; }
"$tools/shell.sh" cap extension   # GNOME-only: stops here, changing nothing, on another shell
: "${PHONE_EXT_DIR:=/usr/share/gnome-shell/extensions/gnome-phone@yesman.de}"
: "${PHONE_SESSION_SERVICE:=$("$tools/shell.sh" get session_service)}"
: "${PHONE_SCHEMA_DIR:=/usr/share/glib-2.0/schemas}"

remote() { "$tools/ssh.sh" "$@"; }                 # as the desktop user
rroot() { "$tools/ssh-root.sh" -s "$1"; }          # as root, HOME = the desktop user's home

stamp=$(date +%Y%m%d-%H%M%S)
stage='$HOME/.cache/gnome-phone-deploy'
echo "== uploading extension/ to the phone"
tar -C "$root/extension" --exclude=meson.build -cf - . | remote "rm -rf $stage && mkdir -p $stage && tar -C $stage -xf -"

# the backup must stay out of the extensions directory: the shell would try to load it as a second extension
backup='$HOME/.cache/gnome-phone-backup-'$stamp
echo "== installing into $PHONE_EXT_DIR (backup: $backup)"
rroot "set -e; cp -a $PHONE_EXT_DIR $backup && cp -a $stage/. $PHONE_EXT_DIR/ && chown -R root:root $PHONE_EXT_DIR
find $backup -type d -exec chown --reference=\"\$HOME\" {} +; rm -rf $stage"

# the extension's GSettings schemas go where Neo Settings finds them too (the package installs them there)
if compgen -G "$root/extension/schemas/*.gschema.xml" >/dev/null; then
    echo "== installing the extension's schemas into $PHONE_SCHEMA_DIR"
    rroot "set -e; cp $PHONE_EXT_DIR/schemas/*.gschema.xml $PHONE_SCHEMA_DIR/ && glib-compile-schemas $PHONE_SCHEMA_DIR"
fi

if [ "${1:-}" = --no-restart ]; then echo "== files in place, not restarting"; exit 0; fi

echo "== restarting $PHONE_SESSION_SERVICE (the screen goes black for about 30 s)"
# stop, end every graphical session left on the seat, start: a plain restart can leave the old session holding the
# display, and the new shell then attaches to it, gets no DRM master and the panel keeps its last picture
case "$("$tools/shell.sh" get restart_mode)" in
    stop-terminate-start)
        rroot "systemctl stop $PHONE_SESSION_SERVICE
  for s in \$(loginctl list-sessions --no-legend | awk '{print \$1}'); do
    case \$(loginctl show-session \$s -p Type --value) in wayland|x11) loginctl terminate-session \$s ;; esac
  done
  sleep 2; systemctl start $PHONE_SESSION_SERVICE" || true ;;
    restart) rroot "systemctl restart $PHONE_SESSION_SERVICE" || true ;;
    *) echo "unknown restart_mode in shells.conf" >&2; exit 1 ;;
esac
for _ in $(seq 40); do
    sleep 3
    state=$(remote "gnome-extensions info gnome-phone@yesman.de 2>/dev/null | grep State" 2>/dev/null) && [ -n "$state" ] && break
done
echo "== ${state:-no answer from the phone}"
remote "journalctl --user -b --no-pager -n 40 -o cat /usr/bin/gnome-shell 2>/dev/null | grep -i 'error\|gnome-phone' | tail -15" || true
