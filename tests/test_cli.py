"""CLI boundary checks; no server or external network connection is needed."""
import subprocess
import sys
import tempfile
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

rejected([cli, "inspect", "unused", "--encrypt", "aes256"], "option")
rejected([cli, "restore", "unused", "-d", "unused", "--pack", "index"], "option")
rejected([cli, "remote-list", "unexpected-positional"], "positional")
rejected([cli, "user", "register", "unexpected-positional"], "positional")
rejected([cli, "backup", "unused", "-o", "a", "-o", "b"], "duplicate")
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
    for encryption in ("none", "chacha20", "aes256"):
        archive = root / (encryption + ".bak")
        destination = root / encryption
        subprocess.run([cli, "backup", str(source), "-o", str(archive),
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
print("CLI argument validation and restore-option roundtrips passed.")
