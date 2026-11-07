#!/bin/sh
# Desktop-integration follow-up for a source build. The build produces the
# binary; this puts it and the distrib identity files where the running
# session looks for them — the xdg-desktop-portal warning "App info not found
# for 'impulse'" is exactly the desktop entry missing from these directories.
# No root required: everything lands in the user's home.
#
#   scripts/install-user.sh [build-dir] [prefix]
#
# Defaults: build-dir "build", prefix "$HOME/.local". Re-run any time; every
# step overwrites in place.
set -e
cd "$(dirname "$0")/.."

BUILD="${1:-build}"
PREFIX="${2:-$HOME/.local}"

if [ ! -f "$BUILD/qimgv/impulse" ]; then
    echo "no impulse binary under $BUILD/ — run cmake --build first" >&2
    exit 1
fi

cmake --install "$BUILD" --prefix "$PREFIX"

mkdir -p "$PREFIX/bin"
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$PREFIX/share/applications" || true

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "note: $PREFIX/bin is not on PATH — add it to run 'impulse' from a terminal" ;;
esac
echo "installed into $PREFIX — the command is 'impulse', the menu entry is ImPulse"
