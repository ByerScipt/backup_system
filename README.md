# Backup Studio

A C++17 backup and restore application for Linux, with a command-line client, a Qt 6 desktop interface, and a TCP backup server. It stores directory trees in a single archive, with configurable packing, compression, and encryption.

The archive format, compression, and archive encryption are self-written.
Optional TLS transport uses OpenSSL.

## Features

- **Archive pipeline:** sequential or indexed packing, with optional RLE, Huffman, or LZ77 compression and ChaCha20 or AES-256-CTR encryption.
- **Files and metadata:** archive and restore directory trees, permissions, ownership, and timestamps, including symbolic links and special files where supported by the filesystem and user privileges.
- **Remote backups:** register an account, upload archives, list backups, and download them for restoration. Each account has its own storage directory.
- **Automatic snapshots:** periodic or Linux filesystem-event triggers, with
  per-source retention and cancellation. Snapshots are complete archives.
- **Transfer options:** verified TLS and incremental uploads that reuse matching
  archive blocks while keeping each stored backup independently restorable.
- **Desktop interface:** local and remote backup workflows with background jobs, progress reporting, logs, and cancellation. Local restore includes a conflict preview. Remote pages share connection
  credentials through the signed-in session; backup history supports name/ID search, numeric size sorting,
  copying full IDs, and opening restore with the selected backup.

The GUI uses Chinese labels; CLI commands and messages are in English.

Closing the GUI cancels all active jobs and waits for cleanup.
Client socket waits time out after 30 seconds;
DNS resolution still follows the system resolver's timeout. Cancellation does
not roll back already restored entries or server-side actions already committed.

Ordinary-file hard links within the source tree are preserved. New
archives use BKP2 format version 2, or version 3 for LZ77; the reader accepts
versions 1, 2 and 3 using supported algorithms. Older readers reject versions
they do not support.
Metadata restoration errors fail the operation. Entries already restored may
remain after a failure.
The restored timestamps are atime and mtime; ctime is recorded but cannot be
restored, and creation time (btime), ACLs and extended attributes are not supported.

Version history is in [CHANGELOG.md](CHANGELOG.md).

## Build

Requires Linux, a C++17 compiler with filesystem support, CMake 3.16+, POSIX
threads and OpenSSL 1.1.1+ development headers. Qt 6 Widgets is optional; GUI
interaction tests also use Qt 6 Test, Network and the `openssl` executable.
The CLI, server and core tests do not require Qt.

On Ubuntu or Debian:

```bash
sudo apt install build-essential cmake libssl-dev openssl ca-certificates
sudo apt install qt6-base-dev fonts-noto-cjk  # Optional: desktop interface and CJK fonts
```

From the repository root:

```bash
make
make test
```

The Makefile delegates to CMake and generates `compile_commands.json` for editor
tooling. Build intermediates stay in `build/`; executables go to `bin/`.
Use `make CMAKE_ARGS=-DBACKUP_BUILD_GUI=OFF` for a CLI-only build. If Qt 6 is
unavailable, CMake skips the GUI automatically.

On Windows, run `./Utils/build-wsl.sh` from Git Bash with WSL2 and the Linux
dependencies installed. It builds a separate copy on the Linux filesystem;
native Windows and MinGW builds are not supported.

| Executable | Purpose |
| :--- | :--- |
| `bin/backup-cli` | Local and remote backup commands |
| `bin/backup-server` | TCP account and archive storage service |
| `bin/backup-gui` | Qt 6 desktop application, when enabled |

Run `bin/backup-gui` to open the desktop interface. The CLI and server accept `--help` and `--version`.

## Local backup and restore

The following example uses the included sample files in `exp/`, backs
them up, inspects the archive, and restores them:

```bash
mkdir -p output

bin/backup-cli backup exp -o output/exp.bak \
  --pack index --compress huffman --encrypt chacha20

bin/backup-cli inspect output/exp.bak
bin/backup-cli restore output/exp.bak -d output/restored
```

Encryption prompts for an archive password; restore requires the same password. For scripted use, pass `--key-file FILE` to read the password from a text file. The source directory name is retained, so this example restores `output/restored/exp/notes.txt`.

| Option | Values | Default |
| :--- | :--- | :--- |
| `--pack` | `stream`, `index` | `stream` |
| `--compress` | `none`, `rle`, `huffman`, `lz77` | `none` |
| `--encrypt` | `none`, `chacha20`, `aes256` | `none` |
| `--include` | Source-relative path; repeat for several entries | Whole tree |

An archive must be written outside the source tree, to a path that does not already exist. Restore rejects conflicting paths unless `--overwrite` is supplied. In the GUI, local restore also offers a conflict preview before extraction.

`--include` archives only the named entries below `SOURCE`, and the directories above them are stored as well, so a restore reproduces the original layout; a selected directory keeps its whole subtree. Absolute paths, parent (`..`) segments and entries that do not exist are rejected. The local and remote backup pages in the GUI offer a checkable tree with name, type, content category, size, time, owner/UID and hidden-entry filters.

The selector initially checks the whole tree. Clearing every checkbox disables
confirmation for a nonempty directory; Cancel keeps the previous selection.
Running jobs use a snapshot of their selections, so later form edits apply only
to the next backup.

For periodic snapshots, choose the corresponding mode in the local backup page
or use `watch`. Its output directory must be outside the source tree:

```bash
bin/backup-cli watch exp -o output/snapshots --interval 60 --retain 5
bin/backup-cli watch exp -o output/realtime --realtime --interval 1 --retain 5
```

The first snapshot is immediate. Periodic intervals start after each completed
snapshot; real-time mode waits for the specified quiet period after a change.
`--count N` stops after N snapshots; otherwise use Ctrl+C or the GUI cancel
button. The process must remain running; this does not install a scheduler.
Only automatically named snapshots for the same canonical source path in that
output directory are expired. Other files are kept. A backup or retention
failure stops the job and reports the error; completed snapshots remain valid.

## Remote backup and restore

Plain TCP is the default for trusted networks. Enable TLS on both ends when
transport confidentiality and server authentication are required. Archive
encryption is configured separately on the client.

Start the server in one terminal:

```bash
bin/backup-server --config databackup.conf
```

By default, it listens on `0.0.0.0:8848`, stores data in `./server_data`, allows up to 32 concurrent sessions, and uses a 30-second socket timeout. These settings are configurable in `databackup.conf`; malformed/unknown settings are rejected. Relative storage paths are resolved from the server's working directory. `bind_address` (or `--bind ADDRESS` on the command line) takes an IPv4 literal and restricts the listener; use `127.0.0.1` for a local-only service.

In another terminal:

```bash
bin/backup-cli user register --server 127.0.0.1:8848 --username alice

bin/backup-cli remote-backup exp --server 127.0.0.1:8848 \
  --username alice --name first-backup \
  --pack stream --compress rle --encrypt chacha20

bin/backup-cli remote-list --server 127.0.0.1:8848 --username alice

bin/backup-cli remote-restore BACKUP_ID -d output/remote-restored \
  --server 127.0.0.1:8848 --username alice
```

Replace `BACKUP_ID` with the ID returned by upload or listing. Account passwords authenticate the user; archive passwords decrypt the backup. The CLI prompts for each when required. For scripted use, supply `--account-password-file FILE` and, for encrypted archives, `--key-file FILE`.

Add `--incremental` to `remote-backup` to compare against the account's newest
archive. Incremental mode requires support on the server; ordinary uploads
remain available. An empty history uses a full upload. Matching 512 KiB blocks at the
same offsets are sent as references; other blocks are sent normally. The block
index is bounded to 32 MiB (the first 512 GiB), with a full-transfer fallback
beyond it. Compression shifts and randomized archive encryption can reduce or
eliminate reuse. The server reconstructs and verifies a complete archive, so
restoration never depends on retaining earlier backups. This mode saves network
traffic when blocks match; it still builds and hashes a complete local archive.

For TLS, supply a PEM server certificate chain and its unencrypted private key:

```bash
bin/backup-server --config databackup.conf \
  --tls-cert server-cert.pem --tls-key server-key.pem
bin/backup-cli remote-list --server backup.example.com:8848 \
  --username alice --tls
```

The certificate must cover the exact DNS name or IP used by the client.
`--tls` uses system CA trust; `--tls-ca ca.pem` enables TLS with a custom PEM
trust file. Server configuration also accepts `tls_certificate` and
`tls_private_key`. In the GUI, enable TLS and optionally choose a CA file on
the login page. Verification cannot be disabled; TLS and plaintext clients
must connect to servers using the same mode. Private keys must be readable
only by the appropriate server account.

## Packaging

`Utils/package-wsl.sh` builds the release installers inside WSL2. From Git
Bash, check out the published release tag and choose an output directory:

```bash
git switch --detach v1.11.0
DELIVERY_ROOT="$(pwd)/output/delivery" ./Utils/package-wsl.sh
# WITH_DOCKER=1 also builds the server image
```

The release gate runs the format check, the parallel build, CTest and the
offscreen GUI start before packaging, then extracts every artifact and runs the
installed binaries and the CLI tests again. `ALLOW_DIRTY=1` and
`ALLOW_UNTAGGED=1` are available for a local trial run; the delivered manifest
records the commit, the toolchain and the checksums.

| Artifact | Deployment line |
| :--- | :--- |
| `backup-studio-client_<version>_amd64.deb` | Separated: Qt desktop client plus `backup-cli` |
| `backup-studio-server_<version>_amd64.deb` | Separated: server, systemd unit, `/etc/backup-studio/databackup.conf` |
| `backup-studio_<version>_amd64.deb` | All-in-one: every component, local service on `127.0.0.1` |
| `backup-studio-<version>-ubuntu24.04-x86_64.tar.gz` | All-in-one without dpkg |
| `backup-studio-server-<version>-image.tar` | Separated: `docker load` with `WITH_DOCKER=1` |
| `SHA256SUMS`, `RELEASE-MANIFEST.txt`, `install-notes.md` | Traceability and installation guide |

Packages are built on Ubuntu 24.04 (glibc 2.39) and target Ubuntu 24.04 or
newer; shared-library dependencies (including OpenSSL) are detected during
packaging. Client packages include CA certificates and, when built with the
GUI, Qt 6, its platform plugin and a CJK font. The all-in-one package starts the server through systemd on the
loopback interface, and `packaging/install-notes.md` documents installation,
upgrades and removal. Windows has no native build: `packaging/windows/install-wsl.ps1`
prepares an Ubuntu 24.04 distribution, installs the all-in-one package there
and creates Start Menu shortcuts that run the client through WSLg. The server
can also run as a container; see `docker-compose.yml`.

## Design

**Archive pipeline.** The core scans a directory tree, packs its contents, then applies the selected compression and encryption. File data is processed in chunks, with temporary files between stages; entry metadata remains in memory. Allow disk space for intermediate files. Sequential packing stores metadata with each entry; indexed packing places an offset table after the file data. Both use the custom `BKP2` format.

**Compression.** RLE and Huffman are self-contained self-written stages. LZ77 is the strongest option: it encodes the packed stream in independent 1 MiB blocks, using a 64 KiB sliding window with hash chains and one-step lazy matching, and stores any block verbatim when the token stream would not be smaller, so compression never grows a payload. LZ77 archives are written as `BKP2` version 3; versions 1 and 2 remain readable.

**Restore.** SHA-256 digests cover the packed data and encoded payload. The reader checks sizes, checksums, paths, and decompression bounds before extracting any entries. Directory file descriptors and `*at` calls constrain destination path resolution. Backup output is committed atomically; restore commits entries individually, without whole-directory rollback.

**Network storage.** A framed TCP protocol supports account registration, challenge-response login, and archive transfers. The server runs a worker thread per connection, subject to the configured connection limit. Uploads are checked against their declared size and SHA-256 digest before being committed to the user's storage directory. Clients build archives before upload and download them before extraction.

**Library boundaries.** `backup_core` provides local archive operations through [core.hpp](libs/core.hpp). `backup_network` depends on the core and exposes client and server operations through [network.hpp](libs/network.hpp). The CLI and GUI share these libraries. Private implementation headers stay under `DataBackup/`.

## Development

The code uses `snake_case` filenames, `PascalCase` types, and `camelCase` functions and fields. `AGENTS.md` records team conventions. Formatting uses clang-format 18,
four spaces, an 80-column limit, Allman braces, and braces around control flow.
Install it before configuring CMake to enable:

```bash
cmake --build build --target format
make format-check
```

## Tests

```bash
make test
```

The CTest suite covers:

- SHA-256, cipher known-answer vectors, and LZ77 unit cases: round trips,
  hand-built streams, and malformed stream rejection.
- Complete directory-tree round trips for all 24 packing, compression, and encryption combinations.
- Incorrect passwords, damaged and truncated archives, size bounds, invalid paths, overwrite conflicts, restore previews, and destination path races.
- Account isolation, upload/download round trips, interrupted uploads, cancelled transfers, request ID validation, session recycling, and persistence across server restarts.
- Periodic/real-time snapshots, restart retention, nested additions/deletions,
  encrypted restore, invalid output paths and prompt cancellation.
- Incremental block reuse, reconstructed checksums, cancellation cleanup and
  restoration after removing the base; TLS trusted/untrusted, wrong-identity
  and expired certificates, encrypted error replies and handshake shutdown.
- Buffered disk-full failures, source-to-FIFO races, final-stage cancellation,
  unreadable/malformed account stores, unresponsive peers and malformed responses.
- CLI numeric/option validation and prompt server shutdown with idle/partial
  connections when Python 3 is available.
- GUI local encrypted round trips, real-server registration/upload/list/download,
  search/sorting/selection, session credentials, Chinese validation, overwrite
  confirmation, and multi-job closing/cancellation when Qt 6 is available.
  CTest runs these offscreen; run with `QT_QPA_PLATFORM=xcb` for desktop events.
  Account workflows run with both plaintext and TLS; regressions also cover
  draft-independent deletion, stale replies, hidden subtrees, owner filtering
  and automatic-backup cancellation/restoration.

Fixtures include regular files, empty directories, symbolic links, FIFOs, and
Unix socket nodes. `make test` also runs nine isolated WSL launcher tests;
these do not replace Windows/WSLg interactive acceptance.

An optional memory-limit check backs up and restores a 160 MiB sparse file with a 128 MiB virtual-memory limit per process. It uses indexed packing without compression or encryption:

```bash
DELIVERY_ROOT="$(pwd)/output/benchmark" \
  ./Utils/run_performance_test.sh bin/backup-cli
```

The script compares source and restored hashes and writes timing and memory
measurements to `output/benchmark/output/evidence/`. These files are generated
when the command runs.

## Repository layout

| Path | Contents |
| :--- | :--- |
| `DataBackup/` | Core and network implementations, CLI and server entry points |
| `GUI/` | Qt pages, widgets, dialogs, background jobs, and styles |
| `libs/` | Public core and network interfaces |
| `test/` | Core, CLI, GUI and WSL launcher tests |
| `Utils/` | WSL build and performance-check scripts |
| `exp/` | Sample input; generated output belongs in `output/` outside this tree |
| `bin/`, `build/` | Generated executables and build intermediates (ignored) |
| `databackup.conf` | Default server configuration |
| `DataBackup.mdj` | Editable StarUML model: public interfaces, use cases, sequences and deployment |
| `Makefile`, `CMakeLists.txt` | Build commands and their CMake implementation |
| `.vscode/`, `.clang-format`, `.clang-tidy` | Editor and code-checking settings |
| `packaging/` | Installer assets: desktop entry, icon, systemd units, deb maintainer scripts, Windows WSL2 installer, install notes |
| `Dockerfile`, `docker-compose.yml` | Container build and run for the server |
