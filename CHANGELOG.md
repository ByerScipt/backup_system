# Changelog

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
