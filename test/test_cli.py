"""CLI validation, roundtrips and shutdown against a local server."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

cli, server = sys.argv[1:3]


def rejected(command, reason):
    result = subprocess.run(command, input="", text=True, capture_output=True, timeout=3)
    assert result.returncode != 0 and reason in result.stderr, (command, result)


for value in ("0", "65536", "65537", "-1", "+80", "80junk", "80.5", " 80", "4294967297"):
    rejected([server, "--port", value, "--help"], "port")
    rejected([cli, "remote-list", "--server", "127.0.0.1:" + value, "--username", "user"], "port")

for value in ("0", "-1", "1025", "2junk", "4294967297"):
    rejected([server, "--max-connections", value, "--help"], "max-connections")

for value in ("localhost", "127.0.0.1:8848", "999.1.1.1", "::1", "127.0.0.1 ", ""):
    rejected([server, "--bind", value, "--help"], "bind")

rejected([cli, "inspect", "unused", "--encrypt", "aes256"], "option")
rejected([cli, "restore", "unused", "-d", "unused", "--pack", "index"], "option")
rejected([cli, "remote-list", "unexpected-positional"], "positional")
rejected([cli, "user", "register", "unexpected-positional"], "positional")
rejected([cli, "backup", "unused", "-o", "a", "-o", "b"], "duplicate")
rejected([cli, "backup", "unused", "-o", "a", "--compress", "lz77x"], "compression")
rejected([cli, "backup", "unused", "-o", "a", "--compress", "huffman", "--compress", "lz77"], "duplicate")
rejected([cli, "restore", "unused", "-d", "unused", "--overwrite", "--overwrite"], "duplicate")

for binary in (cli, server):
    assert subprocess.run([binary, "--help"], capture_output=True, timeout=3).returncode == 0
with tempfile.TemporaryDirectory(prefix="backup-cli-test-") as directory:
    root = Path(directory)
    source = root / "source"
    source.mkdir()
    (source / "file").write_bytes(b"restore options fixture")
    key = root / "key"
    key.write_text("test password")
    for compression in ("none", "rle", "huffman", "lz77"):
        for encryption in ("none", "chacha20", "aes256"):
            name = compression + "-" + encryption
            archive = root / (name + ".bak")
            destination = root / name
            subprocess.run([cli, "backup", str(source), "-o", str(archive),
                            "--compress", compression,
                            "--encrypt", encryption, "--key-file", str(key)],
                           check=True, capture_output=True, timeout=5)
            command = [cli, "restore", str(archive), "-d", str(destination),
                       "--key-file", str(key)]
            subprocess.run(command, check=True, capture_output=True, timeout=5)
            restored = destination / "source/file"
            assert restored.read_bytes() == (source / "file").read_bytes()
            restored.write_bytes(b"existing content")
            subprocess.run(command + ["--overwrite"], check=True,
                           capture_output=True, timeout=5)
            assert restored.read_bytes() == (source / "file").read_bytes()

    # --include stores only the selected entries, keeping the structure.
    nested = source / "nested"
    nested.mkdir()
    (nested / "picked.txt").write_bytes(b"picked")
    (nested / "skipped.txt").write_bytes(b"skipped")
    (source / "other.txt").write_bytes(b"other")
    selected_archive = root / "selected.bak"
    subprocess.run([cli, "backup", str(source), "-o", str(selected_archive),
                    "--include", "nested/picked.txt", "--include", "other.txt"],
                   check=True, capture_output=True, timeout=5)
    selected = root / "selected"
    subprocess.run([cli, "restore", str(selected_archive), "-d", str(selected)],
                   check=True, capture_output=True, timeout=5)
    assert (selected / "source/nested/picked.txt").read_bytes() == b"picked"
    assert (selected / "source/other.txt").read_bytes() == b"other"
    assert not (selected / "source/nested/skipped.txt").exists()
    assert not (selected / "source/file").exists()
    rejected([cli, "backup", str(source), "-o", str(root / "escape.bak"),
              "--include", "../escape"], "selected")
    rejected([cli, "backup", str(source), "-o", str(root / "missing.bak"),
              "--include", "missing.txt"], "selected")
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    service = subprocess.Popen([server, "--bind", "127.0.0.1", "--port", str(port),
                               "--storage", str(root / "storage")],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 3
        while True:
            try:
                idle = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                assert service.poll() is None, "server failed to start"
                assert time.monotonic() < deadline, "server did not accept connections"
                time.sleep(0.02)
        with idle, socket.create_connection(("127.0.0.1", port), timeout=0.2) as partial:
            partial.sendall(b"N")
            time.sleep(0.05)
            service.terminate()
            try:
                assert service.wait(timeout=2) == 0, "server shutdown failed"
            except subprocess.TimeoutExpired:
                raise AssertionError("server hangs on shutdown with an idle client")
    finally:
        if service.poll() is None:
            service.kill()
        service.communicate(timeout=2)
print("CLI argument validation and restore-option roundtrips passed.")
