#!/usr/bin/env bash
# Install the newest Brave Origin BETA rpm from Brave's GitHub releases (no repo publishes it), verify its sha256
# sidecar, and point the wrapper and launcher at it. Brave uses one tag stream with channel-specific assets per tag,
# so releases/latest (the release channel) will not do: scan for the newest tag that carries the beta rpm.
set -euo pipefail
read -r ver asset < <(python3 - <<'PY'
import json, re, urllib.request
pat = re.compile(r"^brave-origin-beta-([0-9.]+)-1\.x86_64\.rpm$")
for page in (1, 2, 3):
    url = f"https://api.github.com/repos/brave/brave-browser/releases?per_page=100&page={page}"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    for rel in json.load(urllib.request.urlopen(req, timeout=30)):
        for a in rel.get("assets", []):
            m = pat.match(a["name"])
            if m:
                print(m.group(1), a["name"]); raise SystemExit
raise SystemExit("no brave-origin-beta rpm found")
PY
)
base="https://github.com/brave/brave-browser/releases/download/v$ver"
tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/$asset" "$base/$asset"
curl -fsSL -o "$tmp/$asset.sha256" "$base/$asset.sha256"
[ "$(awk 'NR==1{print $1}' "$tmp/$asset.sha256")" = "$(sha256sum "$tmp/$asset" | awk '{print $1}')" ] || { echo "sha256 mismatch for $asset" >&2; exit 1; }
# Beta rpms are signed with Brave's pre-release key (brave-core-beta.asc), not the release key.
# The rpm requires brave-keyring, which only Brave's own repo carries: add that repo for this install, then drop it.
rpm --import https://brave-browser-rpm-beta.s3.brave.com/brave-core-beta.asc
curl -fsSL -o /etc/yum.repos.d/brave-browser.repo https://brave-browser-rpm-release.s3.brave.com/brave-browser.repo
# dnf skips OpenPGP checks for a local rpm file, so check Brave's signature here; the sha256 only proves integrity.
rpm -K "$tmp/$asset"
dnf -y install brave-keyring "$tmp/$asset"
rm -rf "$tmp"
# The rpm's %post registers /usr/bin/brave-origin as an alternatives link; our wrapper already owns that path.
alternatives --remove-all brave-origin 2>/dev/null || true

dir="$(dirname "$(ls -d /opt/brave.com/*/brave | head -1)")"
sed -i "s#@BRAVE_DIR@#$dir#" /usr/bin/brave-origin
# Our launcher (brave-origin.desktop, the ID mimeapps.list uses) replaces the rpm's own; icons under that name.
find /usr/share/applications \( -name 'brave-origin*.desktop' -o -name 'com.brave.Origin*.desktop' \) ! -name brave-origin.desktop -delete
for f in "$dir"/product_logo_*.png; do
  size="$(basename "$f" | sed -E 's/product_logo_([0-9]+).*/\1/')"
  install -Dm644 "$f" "/usr/share/icons/hicolor/${size}x${size}/apps/brave-origin.png"
done
# The rpm may drop a repo file or a daily cron job for its own updates; the image updates it instead.
rm -f /etc/yum.repos.d/brave-browser*.repo /etc/cron.daily/brave-browser*
echo "brave-origin-beta $ver installed in $dir"
