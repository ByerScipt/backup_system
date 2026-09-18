# Changelog

## 1.6.2 — 2026-09-18

- Consolidate cipher streaming and CLI restore-option handling; skip redundant
  automatic preview when GUI overwrite is explicit. Align progress stage names
  with GUI labels and centralize documentation history and test traceability.
- Check buffered output close failures throughout packing, compression,
  encryption and transfer staging; reject incomplete input reads.
- Reject unreadable or malformed account databases without replacing them.
- Reject source files replaced by FIFOs without blocking on open.
- Honor cancellation before final archive/file publication, during checksums
  and previews, and while clients wait for network I/O. Socket waits have a
  30-second default timeout; DNS still uses the system resolver's timeout.
- Validate response request IDs, upload completion IDs and terminal frames;
  only expose an uploaded backup ID after a valid completion acknowledgement.
- Cancel all active GUI page jobs on window close and wait for cleanup;
  connect preview, registration and listing to cancellation.
- Reject malformed/overflowing CLI numbers, repeated arguments and options
  unsupported by the selected command.
- Add disk-full, FIFO race, cancellation, protocol, account-store, CLI and
  offscreen GUI lifecycle regression tests. Archive and protocol versions
  remain unchanged; Windows/WSLg interactive acceptance is still pending.

## 1.6.1 — 2026-09-16

- Fix the Windows/WSL build script to create a fresh build copy without deleting
  existing repositories. Honor GUEST_DIR and reject existing or unsafe targets.
- Resolve Windows source paths with WSL's wslpath, keep screenshots inside the
  build directory, and print a correctly quoted GUI command for the build user.
- Keep the GCC 13 cstdint fix and LF shell-script checkouts from the team update.
- Add isolated WSL-script regression tests; native Windows/WSLg still requires
  validation on a Windows machine. Archive format and protocol are unchanged.

## 1.6.0 — 2026-09-16

- Preserve ordinary-file hardlink relationships inside the selected tree;
  write BKP2 format 2 and retain format 1 reading.
- Reject incomplete directory scans and inaccessible remote backup listings.
- Report ownership, permission and timestamp restoration failures.
- Close file descriptors when archive synchronization fails.
- Add filesystem failure, hardlink, mutation and compatibility regression cases.
- Document public API contracts and team conventions; use clang-format 18
  with 80 columns, Allman braces and explicit control-flow braces.
- Prepare local course requirements, design and traceable test documentation
  under the ignored docs directory (not included in this repository).

Existing delivery binaries, PDFs and slides are historical until regenerated
and checked.
