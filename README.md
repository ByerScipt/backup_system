# Backup Studio

A C++17 backup and restore application for Linux, with a command-line client, a Qt 6 desktop interface, and a TCP backup server. It stores directory trees in a single archive, with configurable packing, compression, and encryption.

The project implements its archive format, compression, and encryption without external compression or cryptography libraries.

## Features

- **Archive pipeline:** sequential or indexed packing, with optional RLE or Huffman compression and ChaCha20 or AES-256-CTR encryption.
- **Files and metadata:** archive and restore directory trees, permissions, ownership, and timestamps, including symbolic links and special files where supported by the filesystem and user privileges.
- **Remote backups:** register an account, upload archives, list backups, and download them for restoration. Each account has its own storage directory.
- **Desktop interface:** local and remote backup workflows with background jobs, progress reporting, logs, and cancellation. Local restore includes a conflict preview.

The GUI uses Chinese labels; CLI commands and messages are in English.

## Build

Requires Linux, a C++17 compiler with filesystem support, CMake 3.16+, and POSIX threads. Qt 6 Widgets is optional. The CLI, server, and core tests do not require Qt or external compression and cryptography libraries.

On Ubuntu or Debian:

```bash
sudo apt install build-essential cmake
sudo apt install qt6-base-dev fonts-noto-cjk  # Optional: desktop interface and CJK fonts
```

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Alternatively, `./scripts/build.sh` configures, builds, and runs the tests. Use `-DBACKUP_BUILD_GUI=OFF` when configuring a CLI-only build. If Qt 6 is unavailable, CMake skips the GUI automatically.

| Executable | Purpose |
| :--- | :--- |
| `build/backup-cli` | Local and remote backup commands |
| `build/backup-server` | TCP account and archive storage service |
| `build/backup-gui` | Qt 6 desktop application, when enabled |

Run `build/backup-gui` to open the desktop interface. The CLI and server accept `--help` and `--version`.

## Local backup and restore

The following example creates a small source directory, backs it up, inspects the archive, and restores it:

```bash
mkdir -p demo/source
printf 'Backup Studio example\n' > demo/source/notes.txt

build/backup-cli backup demo/source -o demo/source.bak \
  --pack index --compress huffman --encrypt chacha20

build/backup-cli inspect demo/source.bak
build/backup-cli restore demo/source.bak -d demo/restored
```

Encryption prompts for an archive password; restore requires the same password. For scripted use, pass `--key-file FILE` to read the password from a text file. The source directory name is retained, so this example restores `demo/restored/source/notes.txt`.

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
cp deploy/server.conf.example server.conf
build/backup-server --config server.conf
```

By default, it listens on `0.0.0.0:8848`, stores data in `./server_data`, allows up to 32 concurrent sessions, and uses a 30-second socket timeout. These settings are configurable in `server.conf`; relative storage paths are resolved from the server's working directory.

In another terminal:

```bash
build/backup-cli user register --server 127.0.0.1:8848 --username alice

build/backup-cli remote-backup demo/source --server 127.0.0.1:8848 \
  --username alice --name first-backup \
  --pack stream --compress rle --encrypt chacha20

build/backup-cli remote-list --server 127.0.0.1:8848 --username alice

build/backup-cli remote-restore BACKUP_ID -d demo/remote-restored \
  --server 127.0.0.1:8848 --username alice
```

Replace `BACKUP_ID` with the ID returned by upload or listing. Account passwords authenticate the user; archive passwords decrypt the backup. The CLI prompts for each when required. For scripted use, supply `--account-password-file FILE` and, for encrypted archives, `--key-file FILE`.

## Design

**Archive pipeline.** The core scans a directory tree, packs its contents, then applies the selected compression and encryption. File data is processed in chunks, with temporary files between stages; entry metadata remains in memory. Allow disk space for intermediate files. Sequential packing stores metadata with each entry; indexed packing places an offset table after the file data. Both use the custom `BKP2` format.

**Restore.** SHA-256 digests cover the packed data and encoded payload. The reader checks sizes, checksums, paths, and decompression bounds before extracting any entries. Directory file descriptors and `*at` calls constrain destination path resolution. Backup output is committed atomically; restore commits entries individually, without whole-directory rollback.

**Network storage.** A framed TCP protocol supports account registration, challenge-response login, and archive transfers. The server runs a worker thread per connection, subject to the configured connection limit. Uploads are checked against their declared size and SHA-256 digest before being committed to the user's storage directory. Clients build archives before upload and download them before extraction.

**Library boundaries.** `backup_core` provides local archive operations through [core.hpp](include/backup/core.hpp). `backup_network` depends on the core and exposes client and server operations through [network.hpp](include/backup/network.hpp). The CLI and GUI share these libraries. Private implementation headers stay under `src/`.

## Development

The code uses `snake_case` filenames, `PascalCase` types, and `camelCase` functions and fields. Formatting is defined in `.clang-format` and uses clang-format 18. Install it before configuring CMake to enable:

```bash
cmake --build build --target format
cmake --build build --target format-check
```

GitHub Actions checks formatting, builds the project, runs CTest, and starts the GUI with Qt's offscreen platform on Ubuntu.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

The CTest suite covers:

- SHA-256 and cipher known-answer vectors, including encryption stream offsets.
- Complete directory-tree round trips for all 18 packing, compression, and encryption combinations.
- Incorrect passwords, damaged and truncated archives, size bounds, invalid paths, overwrite conflicts, restore previews, and destination path races.
- Account isolation, upload/download round trips, interrupted uploads, cancelled transfers, request ID validation, session recycling, and persistence across server restarts.

Fixtures include regular files, empty directories, symbolic links, FIFOs, and Unix socket nodes.

An optional memory-limit check backs up and restores a 160 MiB sparse file with a 128 MiB virtual-memory limit per process. It uses indexed packing without compression or encryption:

```bash
DELIVERY_ROOT=/tmp/backup-studio-benchmark \
  ./scripts/run_performance_test.sh build/backup-cli
```

The script compares source and restored hashes and writes timing and memory measurements to `/tmp/backup-studio-benchmark/output/evidence/`.

## Repository layout

| Path | Contents |
| :--- | :--- |
| `include/backup/` | Public core and network interfaces |
| `src/core/` | Archive format, filesystem operations, compression, and encryption |
| `src/network/` | Protocol I/O, accounts, storage, client, and server sessions |
| `cli/` | Command parsing and CLI operations |
| `gui/` | Qt pages, widgets, dialogs, background jobs, and styles |
| `server/` | Server executable entry point |
| `tests/` | Local, network, and algorithm tests |
| `scripts/` | Build and performance-check scripts |
| `deploy/` | Example server configuration |
| `Dockerfile` | Container build for the CLI and server |
