#!/bin/bash
# Deploy extension/ to a running phone over ssh, keep a backup of what it replaces, restart the session and report.
# Settings come from .env at the repository root (see .env.example) or the environment.
#   usage: tools/deploy-extension.sh [--no-restart]
set -euo pipefail

# ssh asks for the password by running this script again as its askpass program
if [ "${DEPLOY_ASKPASS:-}" = 1 ]; then
    printf '%s\n' "$PHONE_PASSWORD"
    exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$root/.env" ] && { set -a; . "$root/.env"; set +a; }
: "${PHONE_HOST:?set PHONE_HOST in .env}" "${PHONE_USER:?set PHONE_USER in .env}" "${PHONE_PASSWORD:?set PHONE_PASSWORD in .env}"
: "${PHONE_EXT_DIR:=/usr/share/gnome-shell/extensions/gnome-phone@yesman.de}"
: "${PHONE_SESSION_SERVICE:=gdm}"
: "${PHONE_SCHEMA_DIR:=/usr/share/glib-2.0/schemas}"

export PHONE_PASSWORD DEPLOY_ASKPASS=1 SSH_ASKPASS="$root/tools/deploy-extension.sh" SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-x}
remote() { setsid -w ssh -o StrictHostKeyChecking=no -o ConnectTimeout=15 "$PHONE_USER@$PHONE_HOST" "$@"; }

stamp=$(date +%Y%m%d-%H%M%S)
stage=/home/$PHONE_USER/.cache/gnome-phone-deploy
echo "== uploading extension/ to $PHONE_HOST"
tar -C "$root/extension" --exclude=meson.build -cf - . | remote "rm -rf $stage && mkdir -p $stage && tar -C $stage -xf -"

# the backup must stay out of the extensions directory: the shell would try to load it as a second extension
backup=/home/$PHONE_USER/.cache/gnome-phone-backup-$stamp
echo "== installing into $PHONE_EXT_DIR (backup: $backup)"
remote "sudo -S -p '' sh -ec 'cp -a $PHONE_EXT_DIR $backup && cp -a $stage/. $PHONE_EXT_DIR/ && chown -R root:root $PHONE_EXT_DIR'; rm -rf $stage" <<<"$PHONE_PASSWORD"

# the extension's GSettings schemas go where Neo Settings finds them too (the package installs them there)
if compgen -G "$root/extension/schemas/*.gschema.xml" >/dev/null; then
    echo "== installing the extension's schemas into $PHONE_SCHEMA_DIR"
    remote "sudo -S -p '' sh -ec 'cp $PHONE_EXT_DIR/schemas/*.gschema.xml $PHONE_SCHEMA_DIR/ && glib-compile-schemas $PHONE_SCHEMA_DIR'" <<<"$PHONE_PASSWORD"
fi

if [ "${1:-}" = --no-restart ]; then echo "== files in place, not restarting"; exit 0; fi

echo "== restarting $PHONE_SESSION_SERVICE (the screen goes black for about 30 s)"
# stop, end every graphical session left on the seat, start: a plain restart can leave the old session holding the
# display, and the new shell then attaches to it, gets no DRM master and the panel keeps its last picture
remote "sudo -S -p '' sh -c 'systemctl stop $PHONE_SESSION_SERVICE
  for s in \$(loginctl list-sessions --no-legend | awk \"{print \\\$1}\"); do
    case \$(loginctl show-session \$s -p Type --value) in wayland|x11) loginctl terminate-session \$s ;; esac
  done
  sleep 2; systemctl start $PHONE_SESSION_SERVICE'" <<<"$PHONE_PASSWORD" || true
for _ in $(seq 40); do
    sleep 3
    state=$(remote "gnome-extensions info gnome-phone@yesman.de 2>/dev/null | grep State" 2>/dev/null) && [ -n "$state" ] && break
done
echo "== ${state:-no answer from the phone}"
remote "journalctl --user -b --no-pager -n 40 -o cat /usr/bin/gnome-shell 2>/dev/null | grep -i 'error\|gnome-phone' | tail -15" || true
