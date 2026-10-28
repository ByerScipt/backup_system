# Changelog

## 1.11.0 — 2026-10-28

- Split the navigation into “本地（无需账户）” (local backup/restore) and
  “远程（需要登录）” (remote backup/restore/history), with a 设置 entry at the
  bottom; pages, indices and the archive format stay unchanged.
- Gate the remote group behind a login: `AccountSession` now holds host, port,
  account and password in memory, the remote navigation is disabled while
  signed out, and signing out locks it again and returns to the settings page.
- Move all connection input into the settings page. The three remote pages read
  the signed-in credentials from the session, show the current account and
  reject the action when signed out instead of asking for a server again.
- Rename the bottom navigation entry to 登录 and make both group headers
  clickable: 本地 opens local backup, and 远程 opens the login page when signed
  out or remote backup when signed in.
- Treat the settings form as a draft: editing it does not change the session;
  only a successful login or registration applies the new credentials.
- Add optional login presets: a checkbox remembers the host, port and account
  name (never the password) in `QSettings`, a dropdown refills the form, and a
  delete action removes the local entry.
- Turn the remote-restore backup ID into an editable dropdown filled by a new
  “刷新历史” button that lists the signed-in account's backups on a worker
  thread; the chosen full ID survives a refresh, typed IDs still work, and the
  list is dropped when the account changes.
- Extend the GUI regression with the login gate, the group-header navigation,
  the account hint on remote pages, preset save/select/delete and the
  restore-history dropdown, and add a `--user-workflow` entry so the
  end-to-end flow can run alone while DEF-01 blocks the shared sequence.
- Record the candidate settings for a later iteration (default algorithms,
  recent directories, startup page, scaling/dark theme, session auto-lock, log
  export, connection test) as a documented TODO in `docs/design.md` 4.12.1.

## 1.10.0 — 2026-10-28

- Extend the Qt entry selector with content categories: the tree gains a
  category column and the filter row a category choice that maps regular files
  to image, document, code, video, audio, archive, program or other by
  extension. Directories, symlinks and special entries keep no category, so the
  filter only narrows regular files and ancestor directories stay visible.
- Add a size-band filter (empty, under 1 MB, 1–100 MB, at least 100 MB), extend
  the age filter with 90 days and one year, and add a hidden-entry toggle for
  dot-prefixed names; a hidden name hides its whole subtree.
- Add “check filtered results” and “uncheck filtered results”. They set the
  visible entries only and never force-check a directory, because a checked
  directory archives its whole subtree and would pull in entries the filter
  hid; directory states are recomputed from the children instead.
- Widen the dialog to 1020×620 and rebalance the two action rows so the extra
  filters and buttons fit without clipping; the archive format, the NBKP
  protocol, the CLI and the core stay unchanged.
- Cover category labels, category/size/age/hidden visibility, the filtered
  check/uncheck states and the resulting archive contents with GUI regressions,
  and keep a dialog screenshot as evidence.

## 1.9.1 — 2026-10-28

- Wire entry-selector confirmation and cancellation to the dialog; preserve
  partial selections even when their count matches the number of root entries.
- Show whole-tree selections as checked and disable confirmation after clearing
  a nonempty tree, so clearing cannot silently request a whole-tree backup.
- Snapshot local and remote selections before starting background jobs; later
  form edits no longer change a running backup's inputs.
- Accept normalized selection paths with trailing directory separators.
- Add selector lifecycle, partial/all/empty selection, in-flight source-edit and
  trailing-separator regressions. Keep the archive format and protocol unchanged.

## 1.9.0 — 2026-10-27

- Add entry selection: `BackupOptions::includes` archives only the paths chosen
  below a source directory, keeps the directories above them so the original
  structure survives, and rejects absolute paths, `..` segments, empty names and
  entries that are missing from the source. An empty list keeps the whole tree,
  so existing callers are unaffected.
- Add `BackupEngine::scanSource()`: it walks a source exactly like `create()`
  and reports the relative path, type, size and modification time of every
  entry, without following symlinks.
- Add the repeatable `--include PATH` option to `backup` and `remote-backup`,
  and validate repeatable options against each command's allow-list so the other
  commands reject it.
- Add the Qt entry selector to the local and remote backup pages: a checkable
  tree with name, type, size and modification time, name/type/age filters,
  select-all and clear actions, a live summary, and a selection that belongs to
  one source directory and is dropped when the source path changes.
- Merge the 1.8.1 backend fixes and the course documents into the development
  line; the BKP2 archive format and the NBKP protocol are unchanged.

## 1.8.1 — 2026-10-23

- Consolidate client response validation and remove redundant authentication
  exception conversion.
- Apply explicit server options after configuration loading regardless of
  argument order; reject duplicate options and incomplete configuration reads.
- Clean uploaded archives when either archive or metadata synchronization
  fails, and reject malformed metadata instead of listing it as valid.
- Keep listener ownership in the server thread, poll for shutdown and reclaim
  finished sessions while idle. Bound overload replies and propagate cancellation
  to error responses; release the account lock before network replies.
- Send frame headers and payloads together to avoid delayed-ACK stalls; reuse
  chunk buffers and secure temporary-file cleanup for downloads. Hash uploads
  before authentication so local preparation cannot expire an idle session.
- Reuse stage files when compression or encryption is disabled, preserving
  checksum validation, progress and cancellation without redundant file copies.
- Add configuration, storage-fault, malformed-metadata, capacity/recycling and
  LZ77 output-failure regressions; record the backend audit and performance checks.

## 1.8.0 — 2026-10-14

- Add release installers: `backup-studio-client` (Qt desktop client plus the
  command-line client), `backup-studio-server` (service, systemd unit and
  conffile) and the all-in-one `backup-studio` package, together with an
  all-in-one tarball and a server container image. `Utils/package-wsl.sh`
  builds them from a tagged commit and records checksums and a release
  manifest.
- Add `--bind` and the `bind_address` setting so a server can keep its
  listener on the loopback interface; the all-in-one package uses it for the
  local service that systemd starts on install.
- Add a self-written LZ77 compression stage: independent 1 MiB blocks, a 64 KiB
  sliding window with hash chains and one-step lazy matching, and verbatim
  stored blocks for incompressible input so the payload never grows. LZ77
  archives use BKP2 version 3, while versions 1 and 2 stay readable; expose
  `--compress lz77`, a Qt compression entry and the `compress-lz77`/
  `decompress-lz77` progress stages.
- Extend the editable StarUML model with use-case, three sequence diagrams and
  deployment views; the public-interface class diagram is unchanged. Keep the
  course documents and evidence in the repository's docs directory.
- Flatten public headers into libs/ and sample files into exp/; update include
  paths and examples, keeping generated output outside the sample source tree.
- Stop tracking GitHub workflow configuration; retain it locally and ignore
  .github/ and .workflow in future commits.

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
