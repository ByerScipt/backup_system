#!/usr/bin/env bash
# Docker server image acceptance for backup-studio-server:1.9.0 (WSL2, Ubuntu 24.04).
# Builds nothing; assumes the image exists. Runs compose, then a CLI round trip.
set -euo pipefail

LOG=/home/builder/docker-verify-190.log
exec > >(tee "$LOG") 2>&1

TREE=/home/builder/backup-system-build.40Bet4
CLI="$TREE/bin/backup-cli"
PWFILE=/tmp/acct.pw
SRC=/tmp/demo-src
OUT=/tmp/demo-out

echo "== docker & image =="
docker version --format 'server {{.Server.Version}}'
docker compose version
docker images backup-studio-server:1.9.0

cd "$TREE"
echo "== compose up =="
BACKUP_VERSION=1.9.0 docker compose up -d
sleep 2
docker compose ps
echo "-- server log tail --"
docker logs backup-studio-server 2>&1 | tail -5

echo "== CLI round trip =="
printf 'demo-pass-123\n' > "$PWFILE"
chmod 600 "$PWFILE"
rm -rf "$SRC" "$OUT"
mkdir -p "$SRC/sub"
echo hello > "$SRC/a.txt"
echo world > "$SRC/sub/b.txt"
ln "$SRC/a.txt" "$SRC/hardlink-to-a.txt"
ln -s a.txt "$SRC/symlink-to-a"

"$CLI" user register --server 127.0.0.1:8848 --username demo \
    --account-password-file "$PWFILE"
"$CLI" remote-backup "$SRC" --server 127.0.0.1:8848 --username demo \
    --account-password-file "$PWFILE" --name t1
"$CLI" remote-list --server 127.0.0.1:8848 --username demo \
    --account-password-file "$PWFILE" | tee /tmp/list.out
ID=$(awk 'NR==2 {print $1}' /tmp/list.out)
echo "restoring id=$ID"
"$CLI" remote-restore "$ID" -d "$OUT" --server 127.0.0.1:8848 \
    --username demo --account-password-file "$PWFILE"

echo "== verify restored content =="
RESTORED="$OUT/demo-src"
diff -r "$SRC" "$RESTORED"
[ "$(stat -c %h "$RESTORED/a.txt")" = "2" ] && echo "hardlink preserved (links=2)"
[ -L "$RESTORED/symlink-to-a" ] && echo "symlink preserved"

echo "== volume content (as container user) =="
# Accounts live under /data/server_data/users/<sha256(username)>/ as .bak+.meta
docker exec backup-studio-server find /data/server_data -type f -exec ls -l {} +

echo "== compose down =="
docker compose down
docker volume rm backup-system-build40bet4_backup-data 2>/dev/null \
    || docker volume ls | grep backup-data || true

echo "DOCKER_VERIFY_OK log=$LOG"
