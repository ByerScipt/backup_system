# Changelog

## 1.7.1 — 2026-10-02

- Simplify the six Qt pages: remove repeated headings and explanatory paragraphs,
  shorten labels and actions, and reveal task progress only after work starts.
- Fold logs by default without discarding output; keep detailed errors and
  overwrite confirmation. Simplify shared card construction and identify test
  fields independently of display text.
- Use a fresh light-blue palette, standard Chinese sans-serif fonts and a 16pt
  minimum body size. Widen path forms, increase
  spacing and keep the action/log area outside the scrolling workspace.
- Scale fonts in discrete 2pt steps at 1500x1000, 1800x1200 and 2100x1400;
  keep the size stable within each tier and restore smaller tiers on shrink.
  Apply the tier to styled controls and let history rows fit the larger text.
- Widen navigation with the window (260–420px) and refresh the layout together;
  keep navigation typography and control geometry stable when selected.
  Mark the active page with a rounded blue outline and synchronize the highlight
  on both clicks and programmatic page changes.
- Open aligned combo lists below their fields with consistent text insets and
  a small built-in-format arrow asset, without new runtime dependencies.
- Make navigation rows equally spaced; align port and text fields and remove
  the sidebar version. Keep the light-blue appearance without a settings page.
- Open on a minimal start page with login/registration, account information,
  switching and logout. Clear passwords and cancel remote work on logout;
  ignore stale login completions after credentials change.
- Add confirmed account deletion using authenticated NBKP messages 40/41.
  Refuse any stored files or other authenticated connections; never delete
  backups. Recheck account records before accepting pending login proofs.
- Keep login/registration text stationary and align both password fields in
  one column. Center validation, confirmation and background-error dialogs
  on the main window after layout, keeping them within the available screen.

## 1.7.0 — 2026-10-02

- Match the course layout: DataBackup, GUI, libs, test, Utils, exp and bin;
  add a CMake-backed Makefile, root databackup.conf and editable StarUML overview.
  Update editor, CI, Docker and WSL paths; remove duplicate scripts and diagrams.
- Fix server shutdown with idle/partial sessions by propagating cancellation.
  Reject malformed/unknown configuration settings and empty storage paths.
- Synchronize account records, metadata and uploaded archives before publication;
  synchronize containing directories and clean staging files on failure.
- Redesign the six Qt pages with compact neutral styling and native controls;
  correct Chinese labels, disabled actions and file-dialog/sidebar styling.
- Share remote connection fields in memory; add history search, typed size/date
  sorting, full-ID copy and selected-backup restore. Clear history on account
  changes and ignore stale listing responses.
- Consolidate resource ownership, path pickers, GUI build objects and filesystem
  regression tests; remove redundant small files and directory nesting.
- Add real-server GUI workflow, configuration, storage-sync and prompt-shutdown
  regressions. BKP2 format and NBKP protocol versions remain unchanged.


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
