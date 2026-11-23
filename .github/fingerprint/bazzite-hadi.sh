#!/usr/bin/env bash
# Everything bazzite-hadi takes from outside its own files, one line each; the workflow hashes the output.
set -euo pipefail
echo "base=$(skopeo inspect --raw docker://ghcr.io/ublue-os/bazzite:testing | sha256sum | cut -c1-64)"
echo "brave=$(curl -fsSL -H "Authorization: Bearer $GH_TOKEN" 'https://api.github.com/repos/brave/brave-browser/releases?per_page=100' \
  | jq -r '[.[].assets[].name | select(test("^brave-origin-beta-[0-9.]+-1\\.x86_64\\.rpm$"))][0]')"
echo "lact=$(curl -fsSL 'https://copr.fedorainfracloud.org/api_3/package/?ownername=ilyaz&projectname=LACT&packagename=lact&with_latest_succeeded_build=true' \
  | jq -r '.builds.latest_succeeded.source_package.version // "unknown"')"
# The repo files this image is built from: a change to the VM image alone does not rebuild this one.
echo "files=$(git ls-files -s -- . ':!recipes/vm.yml' ':!recipes/vm-host.yml' ':!files/vm' ':!files/scripts/vm-mesa.sh' \
  ':!files/scripts/build-v4l2loopback.sh' ':!.github/fingerprint/silverblue-vm.sh' ':!*.md' | sha256sum | cut -c1-64)"
