#!/usr/bin/env bash
# Builds Backup Studio inside WSL2 (Ubuntu 24.04) on the ext4 filesystem and
# runs the same gates as CI: format check, parallel build of the CLI, server and
# Qt GUI, CTest, and an offscreen GUI start/capture check.
#
# Run from Git Bash on Windows:   ./scripts/build-wsl.sh
# The script prints a Git Bash GUI launch command after a successful build.
#
# Overridable: DISTRO (Ubuntu-24.04), USER_NAME (builder), GUEST_DIR (a NEW
# absolute WSL directory). Without GUEST_DIR, allocate a unique guest directory.
set -euo pipefail

DISTRO=${DISTRO:-Ubuntu-24.04}
USER_NAME=${USER_NAME:-builder}

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Translate the repository path into the form the distribution sees.
WIN_SRC=$(cygpath -m "$HERE")

echo "building $WIN_SRC inside $DISTRO as $USER_NAME"

MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -u "$USER_NAME" -- \
    env WIN_SRC="$WIN_SRC" GUEST_DIR="${GUEST_DIR:-}" \
    BUILD_DISTRO="$DISTRO" BUILD_USER="$USER_NAME" bash -s <<'EOS'
set -euo pipefail
SRC=$(realpath -e -- "$(wslpath -u "$WIN_SRC")")
test -f "$SRC/CMakeLists.txt" || { echo "Source is unavailable: $SRC" >&2; exit 1; }
if [[ -n "$GUEST_DIR" ]]; then
    [[ "$GUEST_DIR" == /* ]] || { echo "GUEST_DIR must be an absolute WSL path" >&2; exit 1; }
    [[ ! -e "$GUEST_DIR" && ! -L "$GUEST_DIR" ]] || { echo "GUEST_DIR already exists: $GUEST_DIR" >&2; exit 1; }
    DST=$(realpath -m -- "$GUEST_DIR")
    case "$DST" in
        /mnt|/mnt/*) echo "GUEST_DIR must be on the Linux filesystem, not /mnt" >&2; exit 1 ;;
    esac
    case "$DST/" in
        "$SRC/"*) echo "GUEST_DIR must be outside the source tree" >&2; exit 1 ;;
    esac
    # mkdir (without -p) atomically refuses every existing directory or file.
    mkdir -- "$DST" || { echo "GUEST_DIR must be new with an existing parent: $DST" >&2; exit 1; }
else
    DST=$(mktemp -d "$HOME/backup-system-build.XXXXXX")
fi

echo "== syncing sources to ext4 ($DST) =="
tar -C "$SRC" --exclude=./build-msys --exclude=./build --exclude=./.git -cf - . | tar -C "$DST" -xf -
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
QT_QPA_PLATFORM=offscreen BACKUP_GUI_CAPTURE="$DST/build/backup-studio.png" \
    timeout 10s ./build/backup-gui
test -s "$DST/build/backup-studio.png"

echo "== artifacts =="
ls -l build/backup-cli build/backup-server build/backup-gui
echo "BUILD_WSL_OK"
printf -v GUI_COMMAND '%q' "$DST/build/backup-gui"
printf 'Launch from Git Bash: MSYS_NO_PATHCONV=1 wsl.exe -d %q -u %q -- bash -lc %q\n' \
    "$BUILD_DISTRO" "$BUILD_USER" "$GUI_COMMAND"
EOS
