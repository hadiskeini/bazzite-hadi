#!/usr/bin/env bash
# Install the newest Brave Origin BETA rpm from Brave's GitHub releases (Brave's beta repo lags about a week), verify its sha256
# sidecar and signature, and point the wrapper and launcher at it.
set -euo pipefail
# Newest release tag that carries the beta rpm. Tags come from git and the probe from the release download
# servers: neither counts against the GitHub API limit (60 requests an hour per IP on shared CI runners).
ver=""
tags="$(git ls-remote --tags https://github.com/brave/brave-browser 'refs/tags/v1.*' \
  | awk '{print $2}' | grep -v '\^{}$' | sed 's#refs/tags/v##' | grep -E '^1\.[0-9]+\.[0-9]+$')"
# Nightly tags sit one or two minor versions above beta: walk the minors down, probing the five newest patches of each.
for minor in $(cut -d. -f2 <<<"$tags" | sort -rnu | head -6); do
  for tag in $(grep -E "^1\.$minor\." <<<"$tags" | sort -rV | head -5); do
    if curl -fsSIL -o /dev/null "https://github.com/brave/brave-browser/releases/download/v$tag/brave-origin-beta-$tag-1.x86_64.rpm" 2>/dev/null; then
      ver=$tag; break 2
    fi
  done
done
[ -n "$ver" ] || { echo "no brave-origin-beta rpm found in the newest release tags" >&2; exit 1; }
asset="brave-origin-beta-$ver-1.x86_64.rpm"
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

# /opt links to /usr/lib/opt during the build only (BlueBuild optfix): resolve it, so the wrapper does not need the runtime link.
dir="$(dirname "$(readlink -f "$(find /opt/brave.com/ -mindepth 2 -maxdepth 2 -name brave -type f | head -1)")")"
[ -x "$dir/brave" ] || { echo "brave binary not found under /opt/brave.com" >&2; exit 1; }
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
