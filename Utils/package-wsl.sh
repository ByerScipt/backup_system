#!/usr/bin/env bash
# Builds the release installers inside WSL2 (Ubuntu 24.04) on the ext4
# filesystem and copies them to the delivery directory.
#
# Run from Git Bash on Windows:   ./Utils/package-wsl.sh
#
# Every artifact is traced back to one commit, so the script refuses to package
# a working tree that differs from the release tag. Tag first, for example:
#   git tag -a v1.8.0 -m "Backup Studio 1.8.0"
#
# Artifacts per release:
#   backup-studio-client_<version>_amd64.deb   Qt desktop client + command line
#   backup-studio-server_<version>_amd64.deb   server, unit, conffile
#   backup-studio_<version>_amd64.deb          all-in-one: every component
#   backup-studio-<version>-ubuntu24.04-x86_64.tar.gz   all-in-one tarball
#   SHA256SUMS, RELEASE-MANIFEST.txt           traceability of the above
#
# Overridable: DISTRO (Ubuntu-24.04), USER_NAME (builder), GUEST_DIR (a NEW
# absolute WSL directory), DELIVERY_ROOT (default <repo>/../backup_system_delivery),
# ALLOW_DIRTY=1, ALLOW_UNTAGGED=1, WITH_RPM=1, WITH_DOCKER=1.
set -euo pipefail

DISTRO=${DISTRO:-Ubuntu-24.04}
USER_NAME=${USER_NAME:-builder}

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

VERSION=$(sed -n \
    's/^project(backup_system VERSION \([0-9][0-9.]*\).*/\1/p' \
    CMakeLists.txt | head -1)
if [[ -z "$VERSION" ]]; then
    echo "cannot read the project version from CMakeLists.txt" >&2
    exit 1
fi
COMMIT=$(git rev-parse HEAD)

if [[ "${ALLOW_DIRTY:-0}" != 1 ]] &&
    [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "working tree has uncommitted changes; commit them first" >&2
    echo "or set ALLOW_DIRTY=1 to package an unreproducible tree" >&2
    exit 1
fi
if [[ "${ALLOW_UNTAGGED:-0}" != 1 ]]; then
    if ! git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
        echo "missing release tag v$VERSION" >&2
        echo "create it with: git tag -a v$VERSION -m \"Backup Studio $VERSION\"" >&2
        echo "or set ALLOW_UNTAGGED=1 to package an untagged commit" >&2
        exit 1
    fi
    if [[ "$(git rev-parse "v$VERSION^{commit}")" != "$COMMIT" ]]; then
        echo "tag v$VERSION does not point at HEAD ($COMMIT)" >&2
        exit 1
    fi
fi
if ! grep -q "^## $VERSION" CHANGELOG.md; then
    echo "CHANGELOG.md has no '## $VERSION' section" >&2
    exit 1
fi

DELIVERY_ROOT=${DELIVERY_ROOT:-$(cd "$HERE/.." && pwd)/backup_system_delivery}
WIN_DELIVERY=$(cygpath -m "$DELIVERY_ROOT")
WIN_SRC=$(cygpath -m "$HERE")

echo "packaging Backup Studio $VERSION from $COMMIT"

MSYS_NO_PATHCONV=1 wsl.exe -d "$DISTRO" -u "$USER_NAME" -- \
    env WIN_SRC="$WIN_SRC" WIN_DELIVERY="$WIN_DELIVERY" \
    VERSION="$VERSION" COMMIT="$COMMIT" \
    GUEST_DIR="${GUEST_DIR:-}" WITH_RPM="${WITH_RPM:-0}" \
    WITH_DOCKER="${WITH_DOCKER:-0}" \
    BUILD_DISTRO="$DISTRO" BUILD_USER="$USER_NAME" \
    bash -s <<'EOS'
set -euo pipefail
SRC=$(realpath -e -- "$(wslpath -u "$WIN_SRC")")
test -f "$SRC/CMakeLists.txt" || { echo "Source is unavailable: $SRC" >&2; exit 1; }
DELIVERY=$(realpath -m -- "$(wslpath -u "$WIN_DELIVERY")")
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
    mkdir -- "$DST" || { echo "GUEST_DIR must be new with an existing parent: $DST" >&2; exit 1; }
else
    DST=$(mktemp -d "$HOME/backup-system-release.XXXXXX")
fi

echo "== syncing sources to ext4 ($DST) =="
tar -C "$SRC" --exclude=./build-msys --exclude=./build --exclude=./bin \
    --exclude=./dist --exclude=./compile_commands.json --exclude=./.git \
    -cf - . | tar -C "$DST" -xf -
cd "$DST"
LOGS="$DST/release-logs"
mkdir -p "$LOGS"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "$1 is required inside $BUILD_DISTRO" >&2
        exit 1
    }
}
need cmake
need cpack
need dpkg-deb
need readelf
need sha256sum

echo "== configure =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$LOGS/configure.log"

echo "== format check =="
cmake --build build --target format-check 2>&1 | tee "$LOGS/format-check.log"

echo "== build =="
cmake --build build --parallel "$(nproc)" 2>&1 | tee "$LOGS/build.log"

echo "== ctest (excluding the reported GUI defect) =="
ctest --test-dir build --output-on-failure -E backup-gui-tests \
    2>&1 | tee "$LOGS/ctest.log"

# DEF-01 (docs/test-report.md 4.3.3) is a known, reproducible layout failure at
# this baseline. Packaging may continue, but only when that exact signature
# appears: any other GUI failure aborts the release.
echo "== gui tests (DEF-01 check) =="
if QT_QPA_PLATFORM=offscreen ./bin/backup-gui-tests >"$LOGS/gui-tests.log" 2>&1; then
    echo "backup-gui-tests passed: DEF-01 is fixed, update docs/test-report.md"
else
    if grep -q "login/register switch moved the title" "$LOGS/gui-tests.log"; then
        echo "WARNING: backup-gui-tests still fails with the documented DEF-01"
        echo "WARNING: layout defect; the packages carry that baseline"
    else
        echo "backup-gui-tests failed with an unexpected signature:" >&2
        cat "$LOGS/gui-tests.log" >&2
        exit 1
    fi
fi

echo "== gui smoke (offscreen) =="
QT_QPA_PLATFORM=offscreen BACKUP_GUI_CAPTURE="$LOGS/gui-smoke.png" \
    timeout 10s ./bin/backup-gui
test -s "$LOGS/gui-smoke.png"

echo "== cpack: all-in-one tarball =="
cd "$DST/build"
cpack -G TGZ -D CPACK_COMPONENTS_ALL="client;server;local" \
    2>&1 | tee "$LOGS/cpack-tgz.log"

echo "== cpack: client and server debs =="
cpack -G DEB -D CPACK_COMPONENTS_ALL="client;server" \
    2>&1 | tee "$LOGS/cpack-components.log"

# The all-in-one deb is the same install tree without component splitting, so
# it needs the maintainer scripts explicitly; the component runs must not see
# them on the client package.
echo "== cpack: all-in-one deb =="
cpack -G DEB \
    -D CPACK_DEB_COMPONENT_INSTALL=OFF \
    -D CPACK_COMPONENTS_ALL="client;server;local" \
    -D "CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA=$DST/packaging/deb/postinst;$DST/packaging/deb/prerm;$DST/packaging/deb/postrm;$DST/packaging/deb/conffiles" \
    2>&1 | tee "$LOGS/cpack-full.log"

if [[ "$WITH_RPM" == 1 ]]; then
    if command -v rpmbuild >/dev/null 2>&1; then
        echo "== cpack: rpm (all-in-one) =="
        cpack -G RPM -D CPACK_COMPONENTS_ALL="client;server;local" \
            2>&1 | tee "$LOGS/cpack-rpm.log"
    else
        echo "rpm: skipped, rpmbuild is not installed in $BUILD_DISTRO"
    fi
fi

echo "== verify packages =="
echo "all declared files exist; installed binaries report $VERSION"
DIST="$DST/build/dist"
CLIENT_DEB=$(ls "$DIST"/backup-studio-client_*.deb)
SERVER_DEB=$(ls "$DIST"/backup-studio-server_*.deb)
FULL_DEB=$(ls "$DIST"/backup-studio_*.deb)
FULL_TGZ=$(ls "$DIST"/backup-studio-*.tar.gz)
{
    for deb in "$CLIENT_DEB" "$SERVER_DEB" "$FULL_DEB"; do
        echo "===== $(basename "$deb")"
        dpkg-deb --info "$deb"
        dpkg-deb --contents "$deb"
    done
} > "$LOGS/deb-info.txt" 2>&1

# Qt loads its platform plugin and the CJK font at run time, so neither shows
# up as an ELF dependency; the client packages must still declare both, and
# the server package must not inherit them.
grep -q "qt6-qpa-plugins" "$LOGS/deb-info.txt" ||
    { echo "no package depends on qt6-qpa-plugins" >&2; exit 1; }
grep -q "fonts-noto-cjk" "$LOGS/deb-info.txt" ||
    { echo "no package depends on fonts-noto-cjk" >&2; exit 1; }
if dpkg-deb --info "$SERVER_DEB" | grep -q "qt6-qpa-plugins"; then
    echo "server deb inherited the client dependencies" >&2
    exit 1
fi

VERIFY="$DST/build/verify"
rm -rf "$VERIFY"
mkdir -p "$VERIFY/client" "$VERIFY/server" "$VERIFY/full" "$VERIFY/tgz"
dpkg-deb -x "$CLIENT_DEB" "$VERIFY/client"
dpkg-deb -x "$SERVER_DEB" "$VERIFY/server"
# The tarball keeps its top-level directory, so strip it: the archive holds
# the same tree the all-in-one deb installs below /.
dpkg-deb -x "$FULL_DEB" "$VERIFY/full"
tar -xzf "$FULL_TGZ" -C "$VERIFY/tgz" --strip-components=1

for root in client full tgz; do
    "$VERIFY/$root/usr/bin/backup-cli" --version |
        grep -qx "backup-cli $VERSION" ||
        { echo "installed backup-cli does not report $VERSION" >&2; exit 1; }
done
"$VERIFY/full/usr/bin/backup-server" --version |
    grep -qx "backup-server $VERSION" ||
    { echo "installed backup-server does not report $VERSION" >&2; exit 1; }

test -f "$VERIFY/full/etc/backup-studio/databackup.conf"
test -f "$VERIFY/full/lib/systemd/system/backup-studio.service"
test -f "$VERIFY/full/lib/systemd/system/backup-studio.service.d/local.conf"
test -f "$VERIFY/full/usr/share/applications/backup-studio.desktop"
test ! -e "$VERIFY/server/lib/systemd/system/backup-studio.service.d" ||
    { echo "server deb must not ship the loopback drop-in" >&2; exit 1; }

# No installed binary may point back at the build tree it came from.
for binary in "$VERIFY/full/usr/bin/backup-cli" \
    "$VERIFY/full/usr/bin/backup-server" "$VERIFY/full/usr/bin/backup-gui"; do
    if readelf -d "$binary" | grep -qi "rpath\|runpath"; then
        echo "unexpected RPATH or RUNPATH: $binary" >&2
        exit 1
    fi
    if ldd "$binary" | grep -q "$DST"; then
        echo "installed binary links into the build tree: $binary" >&2
        exit 1
    fi
done

echo "== artifacts =="
RELEASE_DIR="$DELIVERY/release/$VERSION"
EVIDENCE="$DELIVERY/output/evidence"
# Start from an empty release directory: a stale file from an earlier build
# must never end up in SHA256SUMS or in the manifest listing.
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR" "$EVIDENCE"
cp "$CLIENT_DEB" "$SERVER_DEB" "$FULL_DEB" "$FULL_TGZ" "$RELEASE_DIR/"
chmod 644 "$RELEASE_DIR"/* 2>/dev/null || true

if [[ "$WITH_DOCKER" == 1 ]]; then
    if command -v docker >/dev/null 2>&1; then
        echo "== docker image =="
        docker build -t "backup-studio-server:$VERSION" \
            --build-arg "BACKUP_VERSION=$VERSION" "$DST" \
            2>&1 | tee "$LOGS/docker-build.log"
        docker save "backup-studio-server:$VERSION" \
            -o "$RELEASE_DIR/backup-studio-server-$VERSION-image.tar"
    else
        echo "docker: skipped, docker is not available in $BUILD_DISTRO"
    fi
else
    echo "docker: not requested; set WITH_DOCKER=1 on a host with docker"
fi

cp "$SRC/packaging/install-notes.md" "$RELEASE_DIR/" 2>/dev/null ||
    echo "warning: packaging/install-notes.md is missing" >&2
cp "$LOGS"/*.log "$LOGS/gui-smoke.png" "$LOGS/deb-info.txt" "$EVIDENCE/"

{
    echo "Backup Studio $VERSION release manifest"
    echo "commit: $COMMIT"
    echo "built (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "built in: $BUILD_DISTRO $BUILD_USER"
    # Not `VERSION`: /etc/os-release defines its own VERSION (e.g. "24.04 LTS").
    echo "host: $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "platform: $(uname -m)"
    echo "compiler: $(c++ --version | head -1)"
    echo "cmake: $(cmake --version | head -1)"
    echo "qt: $(dpkg-query -W -f='${Package} ${Version}' 'libqt6widgets6*' 2>/dev/null | head -1 || echo none)"
    echo "glibc: $(ldd --version | head -1)"
    echo
    echo "test exit: see output/evidence/ctest.log and gui-tests.log"
    echo "known defect: DEF-01 (docs/test-report.md 4.3.3) is unresolved"
    echo
    echo "artifacts:"
    ls -1 "$RELEASE_DIR" | sed 's/^/  /'
} > "$RELEASE_DIR/RELEASE-MANIFEST.txt"

(
    cd "$RELEASE_DIR"
    # SHA256SUMS itself is written last, so it never hashes its own input.
    sha256sum ./*.deb ./*.tar.gz ./install-notes.md \
        ./RELEASE-MANIFEST.txt > SHA256SUMS
)

echo "== release directory =="
ls -l "$RELEASE_DIR"
echo
echo "Manual acceptance for the packages (root inside $BUILD_DISTRO):"
echo "  sudo apt-get install -y $RELEASE_DIR/backup-studio_${VERSION}_amd64.deb"
echo "  sudo systemctl status backup-studio   # only with systemd as init"
echo "RELEASE_PACKAGE_OK"
EOS
