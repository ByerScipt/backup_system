#!/usr/bin/env bash
set -euo pipefail

CLI="${1:-build/backup-cli}"
ROOT="${2:-/tmp/backup-system-performance}"
MEMORY_KIB="${MEMORY_KIB:-131072}"
SIZE_MIB="${SIZE_MIB:-160}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DELIVERY_ROOT="${DELIVERY_ROOT:-$PROJECT_ROOT/../backup_system_delivery}"
EVIDENCE="$DELIVERY_ROOT/output/evidence"

if [[ "$CLI" != /* ]]; then CLI="$PROJECT_ROOT/$CLI"; fi

case "$ROOT" in
  /tmp/backup-system-*) ;;
  *) echo "Refusing to clean unsafe performance path: $ROOT" >&2; exit 2 ;;
esac

mkdir -p "$EVIDENCE"
rm -rf "$ROOT"
mkdir -p "$ROOT/source" "$ROOT/restored"
truncate -s "${SIZE_MIB}M" "$ROOT/source/large.bin"

START_NS="$(date +%s%N)"
/usr/bin/time -v -o "$EVIDENCE/performance-backup.time" \
  bash -c 'ulimit -v "$1"; exec "$2" backup "$3" -o "$4" --pack index --compress none --encrypt none' \
  _ "$MEMORY_KIB" "$CLI" "$ROOT/source" "$ROOT/large.bak" \
  >"$EVIDENCE/performance-backup.log" 2>&1
/usr/bin/time -v -o "$EVIDENCE/performance-restore.time" \
  bash -c 'ulimit -v "$1"; exec "$2" restore "$3" -d "$4"' \
  _ "$MEMORY_KIB" "$CLI" "$ROOT/large.bak" "$ROOT/restored" \
  >"$EVIDENCE/performance-restore.log" 2>&1
END_NS="$(date +%s%N)"

SOURCE_SHA="$(sha256sum "$ROOT/source/large.bin" | cut -d' ' -f1)"
RESTORED_SHA="$(sha256sum "$ROOT/restored/source/large.bin" | cut -d' ' -f1)"
test "$SOURCE_SHA" = "$RESTORED_SHA"

ARCHIVE_BYTES="$(stat -c %s "$ROOT/large.bak")"
BACKUP_RSS="$(awk -F: '/Maximum resident set size/ {gsub(/^[ \t]+/,"",$2); print $2}' "$EVIDENCE/performance-backup.time")"
RESTORE_RSS="$(awk -F: '/Maximum resident set size/ {gsub(/^[ \t]+/,"",$2); print $2}' "$EVIDENCE/performance-restore.time")"
ELAPSED_MS="$(( (END_NS - START_NS) / 1000000 ))"

{
  echo "Performance acceptance evidence"
  echo "Input MiB: $SIZE_MIB"
  echo "Virtual-memory limit KiB: $MEMORY_KIB"
  echo "Archive bytes: $ARCHIVE_BYTES"
  echo "Backup maximum RSS: $BACKUP_RSS"
  echo "Restore maximum RSS: $RESTORE_RSS"
  echo "Roundtrip elapsed ms: $ELAPSED_MS"
  echo "Source SHA-256: $SOURCE_SHA"
  echo "Restored SHA-256: $RESTORED_SHA"
  echo "Result: PASS"
} | tee "$EVIDENCE/performance-summary.txt"
