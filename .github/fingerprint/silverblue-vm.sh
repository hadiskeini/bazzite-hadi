#!/usr/bin/env bash
# Everything silverblue-vm takes from outside its own files, one line each; the workflow hashes the output.
# A new kernel changes the base digest, so the v4l2loopback module is rebuilt with it; new Mesa or module
# versions alone come in with the weekly build.
set -euo pipefail
echo "base=$(skopeo inspect --raw docker://quay.io/fedora/fedora-silverblue:45 | sha256sum | cut -c1-64)"
echo "umbriel=$(for r in umbriel xdg-desktop-portal-umbriel noctalia; do git ls-remote "https://github.com/noctalia-dev/$r" HEAD | cut -c1-12; done | paste -sd, -)"
echo "files=$(git ls-files -s -- recipes/vm.yml recipes/vm-host.yml recipes/umbriel.yml files/vm files/umbriel \
  files/scripts/build-umbriel.sh files/scripts/build-v4l2loopback.sh files/scripts/vm-mesa.sh \
  .github/workflows/image.yml .github/fingerprint/silverblue-vm.sh | sha256sum | cut -c1-64)"
