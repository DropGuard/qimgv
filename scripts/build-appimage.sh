#!/bin/sh
# Builds the self-contained AppImage inside a ubuntu:22.04 container (the
# glibc floor the bundle inherits: 2.35 — covers Debian 12+ and Ubuntu 22.04+).
# Everything the user's distribution would have to provide rides inside:
# Qt 6.9, OpenCV, exiv2, libmpv and the engine. Requires docker; no FUSE
# needed (appimagetool runs in extract-and-run mode).
#
#   scripts/build-appimage.sh [output-dir]
#
# The AppImage lands in the repository root (or the given directory).
set -e
cd "$(dirname "$0")/.."

VERSION=$(grep -oP 'appVersion\(\K[0-9,]+' qimgv/appversion.cpp | tr ',' '.')
OUT="${1:-.}"

# the inner script is a file, not a quoted string: the APPRUN heredoc inside
# it contains single quotes that cannot survive a bash -c '...' wrapper
cat > .appimage-inner.sh <<'INNER'
#!/bin/bash
set -euo pipefail
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git curl ca-certificates \
  build-essential cmake ninja-build python3 python3-venv python3-pip file \
  libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 libxcb1-dev libxcb-cursor0 \
  libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
  libxcb-shape0 libxcb-xinerama0 libopencv-dev libexiv2-dev libmpv-dev g++-12
curl -sL https://rustup.rs | sh -s -- -y --default-toolchain stable
. "$HOME/.cargo/env"
python3 -m venv /opt/aqt-venv
/opt/aqt-venv/bin/pip install -q aqtinstall
/opt/aqt-venv/bin/aqt install-qt linux desktop 6.9.1 linux_gcc_64 -O /opt/qt

cmake -S . -B build-appimage -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/qt/6.9.1/gcc_64
cmake --build build-appimage -j$(nproc)

mkdir -p AppDir/usr/bin/plugins AppDir/usr/share/applications \
  AppDir/usr/share/metainfo AppDir/usr/share/impulse/translations \
  AppDir/usr/share/icons/hicolor/scalable/apps
cp build-appimage/qimgv/impulse AppDir/usr/bin/
cp build-appimage/plugins/player_mpv/player_mpv.so AppDir/usr/bin/plugins/
cp qimgv/distrib/impulse.desktop AppDir/usr/share/applications/
cp qimgv/distrib/impulse.appdata.xml AppDir/usr/share/metainfo/
cp qimgv/distrib/hicolor/scalable/apps/impulse.svg AppDir/usr/share/icons/hicolor/scalable/apps/
cp build-appimage/qimgv/translations/*.qm AppDir/usr/share/impulse/translations/

curl -sL -o linuxdeploy.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sL -o appimagetool.AppImage https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x linuxdeploy.AppImage appimagetool.AppImage
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="/opt/qt/6.9.1/gcc_64/bin:$PATH"
./linuxdeploy.AppImage --appdir AppDir

# Qt plugins beside the binary, found through a qt.conf relative to the
# executable — no AppRun script, no environment needed
mkdir -p AppDir/usr/plugins
for d in platforms imageformats iconengines; do
    cp -r "/opt/qt/6.9.1/gcc_64/plugins/$d" AppDir/usr/plugins/
done
printf "[Paths]\nPlugins=../plugins\n" > AppDir/usr/bin/qt.conf

# linuxdeploy leaves AppRun as a symlink to the binary; replace it with a
# script that points the loader and the plugin path at the bundle
rm -f AppDir/AppRun
cat > AppDir/AppRun <<'APPRUN'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
exec "$HERE/usr/bin/impulse" "$@"
APPRUN
chmod +x AppDir/AppRun

./appimagetool.AppImage --appimage-extract-and-run AppDir "ImPulse-${VERSION}-x86_64.AppImage"
INNER

docker run --rm -e VERSION="$VERSION" -v "$PWD:/repo" -w /repo ubuntu:22.04 bash /repo/.appimage-inner.sh
rm -f .appimage-inner.sh
ls -la "$OUT/ImPulse-${VERSION}-x86_64.AppImage"
