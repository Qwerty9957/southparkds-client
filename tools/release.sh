#!/usr/bin/env bash
# Builds, icon-patches, and publishes a new SouthparkDS client release:
#   - rebuilds the .nds with the Windows devkitPro/calico toolchain (via msys2)
#   - re-applies the Kyle banner icon (ndstool silently drops gfx/icon.bmp)
#   - commits source changes, tags vX.Y.Z, pushes, uploads a GitHub release
#     with the .nds attached, and finally ships the .nds to the DSi over FTP
#
# Run from WSL (needs gh, python3, git). Usage:
#   tools/release.sh [vX.Y.Z]        # default: bump the patch of the last tag
set -euo pipefail
cd "$(dirname "$0")/.."

# -- version ---------------------------------------------------------------
if [ "${1:-}" ]; then
  VER="${1#v}"
else
  LAST=$(git describe --tags --abbrev=0 2>/dev/null || echo v0.0.0)
  LAST="${LAST#v}"
  VER="${LAST%.*}.$(( ${LAST##*.} + 1 ))"
fi
TAG="v$VER"
echo "==> release $TAG"

# -- build (Windows toolchain via msys2) -----------------------------------
echo "==> building (msys2 make)"
/mnt/c/devkitPro/msys2/usr/bin/bash.exe -lc \
  'cd /c/Users/camer/SouthparkDS && export DEVKITPRO=/c/devkitPro DEVKITARM=/c/devkitPro/devkitARM LIBNDS=/c/devkitPro/libnds && make clean && make' \
  > /dev/null

# -- banner icon -----------------------------------------------------------
echo "==> patching banner icon"
python3 gfx/patchicon.py SouthparkDS.nds gfx/icon.bmp SouthparkDS.nds

# -- commit / tag / push ---------------------------------------------------
git add -A
if ! git diff --cached --quiet; then
  git commit -q -m "$TAG"
fi
git tag "$TAG"
git push origin main
git push origin "$TAG"

# -- GitHub release --------------------------------------------------------
echo "==> creating GitHub release"
gh release create "$TAG" SouthparkDS.nds \
  --title "$TAG" \
  --notes "SHA $(git rev-parse --short HEAD)" \
  --repo Qwerty9957/southparkds-client

# -- push to the DSi -------------------------------------------------------
echo "==> uploading to DSi"
curl -s -T SouthparkDS.nds ftp://10.0.0.247:5000/SouthparkDS.nds --user anonymous:
curl -s -o /tmp/opencode/verify.nds ftp://10.0.0.247:5000/SouthparkDS.nds --user anonymous:
if ! cmp -s SouthparkDS.nds /tmp/opencode/verify.nds; then
  echo "!! DSi upload mismatch" >&2
  exit 1
fi
echo "==> done: https://github.com/Qwerty9957/southparkds-client/releases/tag/$TAG"