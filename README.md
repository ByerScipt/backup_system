# Backup Studio

A C++17 backup and restore application for Linux, with a command-line client, a Qt 6 desktop interface, and a TCP backup server. It stores directory trees in a single archive, with configurable packing, compression, and encryption.

The project implements its archive format, compression, and encryption without external compression or cryptography libraries.

## Features

- **Archive pipeline:** sequential or indexed packing, with optional RLE or Huffman compression and ChaCha20 or AES-256-CTR encryption.
- **Files and metadata:** archive and restore directory trees, permissions, ownership, and timestamps, including symbolic links and special files where supported by the filesystem and user privileges.
- **Remote backups:** register an account, upload archives, list backups, and download them for restoration. Each account has its own storage directory.
- **Desktop interface:** local and remote backup workflows with background jobs, progress reporting, logs, and cancellation. Local restore includes a conflict preview. Remote pages share connection
  fields in memory; backup history supports name/ID search, numeric size sorting,
  copying full IDs, and opening restore with the selected backup.

The GUI uses Chinese labels; CLI commands and messages are in English.

Closing the GUI cancels all active jobs and waits for cleanup.
Client socket waits time out after 30 seconds;
DNS resolution still follows the system resolver's timeout. Cancellation does
not roll back already restored entries or server-side actions already committed.

Ordinary-file hard links within the source tree are preserved. New
archives use BKP2 format version 2; the reader also accepts version 1 archives
using supported algorithms. Older applications reject version 2 archives.
Metadata restoration errors fail the operation. Entries already restored may
remain after a failure.
The restored timestamps are atime and mtime; ctime is recorded but cannot be
restored, and creation time (btime), ACLs and extended attributes are not supported.

Version history is in [CHANGELOG.md](CHANGELOG.md). The adjacent delivery output contains historical
artifacts and must be regenerated before submission.

## Build

Requires Linux, a C++17 compiler with filesystem support, CMake 3.16+, and POSIX threads. Qt 6 Widgets is optional; GUI interaction tests also use Qt 6 Test and Network. The CLI, server, and core tests do not require Qt or external compression and cryptography libraries.

On Ubuntu or Debian:

```bash
sudo apt install build-essential cmake
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

The following example uses the included sample files in `exp/source/`, backs
them up, inspects the archive, and restores them:

```bash
mkdir -p exp/output

bin/backup-cli backup exp/source -o exp/output/source.bak \
  --pack index --compress huffman --encrypt chacha20

bin/backup-cli inspect exp/output/source.bak
bin/backup-cli restore exp/output/source.bak -d exp/output/restored
```

Encryption prompts for an archive password; restore requires the same password. For scripted use, pass `--key-file FILE` to read the password from a text file. The source directory name is retained, so this example restores `exp/output/restored/source/notes.txt`.

| Option | Values | Default |
| :--- | :--- | :--- |
| `--pack` | `stream`, `index` | `stream` |
| `--compress` | `none`, `rle`, `huffman` | `none` |
| `--encrypt` | `none`, `chacha20`, `aes256` | `none` |

An archive must be written outside the source tree, to a path that does not already exist. Restore rejects conflicting paths unless `--overwrite` is supplied. In the GUI, local restore also offers a conflict preview before extraction.

## Remote backup and restore

The server is intended for trusted-network use. Its TCP protocol does not provide TLS; archive encryption is configured separately on the client.

Start the server in one terminal:

```bash
bin/backup-server --config databackup.conf
```

By default, it listens on `0.0.0.0:8848`, stores data in `./server_data`, allows up to 32 concurrent sessions, and uses a 30-second socket timeout. These settings are configurable in `databackup.conf`; malformed/unknown settings are rejected. Relative storage paths are resolved from the server's working directory.

In another terminal:

```bash
bin/backup-cli user register --server 127.0.0.1:8848 --username alice

bin/backup-cli remote-backup exp/source --server 127.0.0.1:8848 \
  --username alice --name first-backup \
  --pack stream --compress rle --encrypt chacha20

bin/backup-cli remote-list --server 127.0.0.1:8848 --username alice

bin/backup-cli remote-restore BACKUP_ID -d exp/output/remote-restored \
  --server 127.0.0.1:8848 --username alice
```

Replace `BACKUP_ID` with the ID returned by upload or listing. Account passwords authenticate the user; archive passwords decrypt the backup. The CLI prompts for each when required. For scripted use, supply `--account-password-file FILE` and, for encrypted archives, `--key-file FILE`.

## Design

**Archive pipeline.** The core scans a directory tree, packs its contents, then applies the selected compression and encryption. File data is processed in chunks, with temporary files between stages; entry metadata remains in memory. Allow disk space for intermediate files. Sequential packing stores metadata with each entry; indexed packing places an offset table after the file data. Both use the custom `BKP2` format.

**Restore.** SHA-256 digests cover the packed data and encoded payload. The reader checks sizes, checksums, paths, and decompression bounds before extracting any entries. Directory file descriptors and `*at` calls constrain destination path resolution. Backup output is committed atomically; restore commits entries individually, without whole-directory rollback.

**Network storage.** A framed TCP protocol supports account registration, challenge-response login, and archive transfers. The server runs a worker thread per connection, subject to the configured connection limit. Uploads are checked against their declared size and SHA-256 digest before being committed to the user's storage directory. Clients build archives before upload and download them before extraction.

**Library boundaries.** `backup_core` provides local archive operations through [core.hpp](libs/backup/core.hpp). `backup_network` depends on the core and exposes client and server operations through [network.hpp](libs/backup/network.hpp). The CLI and GUI share these libraries. Private implementation headers stay under `DataBackup/`.

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

- SHA-256 and cipher known-answer vectors, including encryption stream offsets.
- Complete directory-tree round trips for all 18 packing, compression, and encryption combinations.
- Incorrect passwords, damaged and truncated archives, size bounds, invalid paths, overwrite conflicts, restore previews, and destination path races.
- Account isolation, upload/download round trips, interrupted uploads, cancelled transfers, request ID validation, session recycling, and persistence across server restarts.
- Buffered disk-full failures, source-to-FIFO races, final-stage cancellation,
  unreadable/malformed account stores, unresponsive peers and malformed responses.
- CLI numeric/option validation and prompt server shutdown with idle/partial
  connections when Python 3 is available.
- GUI local encrypted round trips, real-server registration/upload/list/download,
  search/sorting/selection, shared credentials, Chinese validation, overwrite
  confirmation, and multi-job closing/cancellation when Qt 6 is available.
  CTest runs these offscreen; run with `QT_QPA_PLATFORM=xcb` for desktop events.

Fixtures include regular files, empty directories, symbolic links, FIFOs, and
Unix socket nodes. `make test` also runs the seven isolated WSL launcher tests;
these do not replace Windows/WSLg interactive acceptance.

An optional memory-limit check backs up and restores a 160 MiB sparse file with a 128 MiB virtual-memory limit per process. It uses indexed packing without compression or encryption:

```bash
DELIVERY_ROOT=/tmp/backup-studio-benchmark \
  ./Utils/run_performance_test.sh bin/backup-cli
```

The script compares source and restored hashes and writes timing and memory measurements to `/tmp/backup-studio-benchmark/output/evidence/`.

## Repository layout

| Path | Contents |
| :--- | :--- |
| `DataBackup/` | Core and network implementations, CLI and server entry points |
| `GUI/` | Qt pages, widgets, dialogs, background jobs, and styles |
| `libs/backup/` | Public core and network interfaces |
| `test/` | Core, CLI, GUI and WSL launcher tests |
| `Utils/` | WSL build and performance-check scripts |
| `exp/source/` | Sample input; generated output belongs in `exp/output/` |
| `bin/`, `build/` | Generated executables and build intermediates (ignored) |
| `databackup.conf` | Default server configuration |
| `DataBackup.mdj` | Editable StarUML public-interface class diagram |
| `Makefile`, `CMakeLists.txt` | Build commands and their CMake implementation |
| `.vscode/`, `.clang-format`, `.clang-tidy` | Editor and code-checking settings |
| `Dockerfile` | Container build for the CLI and server |
