#!/usr/bin/env bash
# Builds Backup Studio inside WSL2 (Ubuntu 24.04) on the ext4 filesystem and
# runs the same gates as CI: format check, parallel build of the CLI, server and
# Qt GUI, CTest, and an offscreen GUI start/capture check.
#
# Run from Git Bash on Windows:   ./scripts/build-wsl.sh
# Launch the GUI interactively:   wsl -d Ubuntu-24.04 -- ~/backup_system/build/backup-gui
#
# Overridable: DISTRO (Ubuntu-24.04), USER_NAME (builder), GUEST_DIR.
set -euo pipefail

DISTRO=${DISTRO:-Ubuntu-24.04}
USER_NAME=${USER_NAME:-builder}

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Translate the repository path into the form the distribution sees.
WSL_SRC=$(cygpath -m "$HERE" | sed -E 's|^([A-Za-z]):|/mnt/\L\1|')

echo "building $WSL_SRC inside $DISTRO as $USER_NAME"

MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -u "$USER_NAME" -- \
    env SRC="$WSL_SRC" bash -s <<'EOS'
set -euo pipefail
DST="$HOME/backup_system"

echo "== syncing sources to ext4 ($DST) =="
rm -rf "$DST"
mkdir -p "$DST"
tar -C "$SRC" --exclude=./build-msys --exclude=./build -cf - . | tar -C "$DST" -xf -
cd "$DST"

echo "== configure =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo "== format check =="
cmake --build build --target format-check

echo "== build =="
cmake --build build --parallel "$(nproc)"

echo "== ctest =="
ctest --test-dir build --output-on-failure

echo "== gui smoke (offscreen) =="
rm -f /tmp/backup-studio.png
QT_QPA_PLATFORM=offscreen BACKUP_GUI_CAPTURE=/tmp/backup-studio.png \
    timeout 10s ./build/backup-gui
test -s /tmp/backup-studio.png

echo "== artifacts =="
ls -l build/backup-cli build/backup-server build/backup-gui
echo "BUILD_WSL_OK"
EOS
