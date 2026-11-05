#!/usr/bin/env bash
# silverblue-vm: the Mac's Touch ID first, the password after, for sudo, sudo -i and polkit (upstream's
# touchid.sh "on", OmacVM ADR 0041). The key /etc/omacvm/touchid-key is a secret: it is put on the VM by hand.
set -euo pipefail
mark="# omacvm touch-id (ADR 0041): the Mac's Touch ID first, the password after"
line='auth       sufficient   pam_exec.so quiet seteuid stdout /usr/libexec/omacvm-touchid'
for f in /etc/pam.d/sudo /etc/pam.d/sudo-i /usr/lib/pam.d/polkit-1; do
  [[ -f $f ]] || { echo "no $f" >&2; exit 1; }
  awk -v m="$mark" -v l="$line" '!d && /^[[:space:]]*-?auth[[:space:]]/ { print m; print l; d = 1 } { print }' "$f" > "$f.new"
  grep -qxF "$line" "$f.new" || { echo "no auth line in $f" >&2; exit 1; }
  chmod 644 "$f.new" && mv -f "$f.new" "$f"
done
echo "Touch ID lines in sudo, sudo-i and polkit-1"
