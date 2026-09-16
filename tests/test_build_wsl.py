"""Exercise the real launcher with isolated Windows/WSL/build command doubles.

No Windows host, real build, deletion, or HOME override is involved.
Run: python3 tests/test_build_wsl.py
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build-wsl.sh"
STUB = r'''#!/usr/bin/python3
import os, pathlib, sys, tempfile
name = pathlib.Path(sys.argv[0]).name
root = pathlib.Path(os.environ["WSL_TEST_ROOT"])
if name == "cygpath":
    print(root / "source space")
elif name == "wslpath":
    print(sys.argv[-1])
elif name == "wsl.exe":
    args = sys.argv[sys.argv.index("--") + 1:]
    os.execvp(args[0], args)
elif name == "mktemp":
    print(tempfile.mkdtemp(prefix="generated-", dir=root))
elif name == "cmake":
    if os.environ.get("WSL_TEST_BUILD_FAIL"):
        sys.exit(7)
    build = pathlib.Path("build")
    build.mkdir(exist_ok=True)
    for binary in ("backup-cli", "backup-server", "backup-gui"):
        p = build / binary
        p.write_text("#!/usr/bin/python3\nimport os,pathlib\np=os.environ.get('BACKUP_GUI_CAPTURE')\nif p: pathlib.Path(p).write_bytes(b'fixture')\n")
        p.chmod(0o755)
elif name == "nproc":
    print(2)
elif name == "ctest":
    pass
elif name == "rm":
    raise SystemExit("Unexpected deletion attempted")
'''


class BuildWslTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="backup-wsl-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source space"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text("# fixture\n")
        (self.source / "keep.txt").write_text("source survives")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("cygpath", "wslpath", "wsl.exe", "mktemp", "cmake", "ctest", "nproc", "rm"):
            p = self.bin / name
            p.write_text(STUB)
            p.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        WSL_TEST_ROOT=str(self.root), GUEST_DIR="", DISTRO="Test Distro", USER_NAME="test-builder")

    def run_script(self, **env):
        return subprocess.run(["bash", str(SCRIPT)], env=dict(self.env, **env),
                              text=True, capture_output=True, timeout=20)

    def test_existing_directory_preserved(self):
        target = self.root / "existing"
        target.mkdir()
        marker = target / "uncommitted.txt"
        marker.write_text("keep")
        result = self.run_script(GUEST_DIR=str(target))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(marker.read_text(), "keep")

    def test_custom_path_and_gui_command(self):
        target = self.root / "custom space 'quoted' $literal"
        result = self.run_script(GUEST_DIR=str(target))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((target / "keep.txt").read_text(), "source survives")
        self.assertTrue((target / "build/backup-studio.png").is_file())
        command = result.stdout.split("Launch from Git Bash: ", 1)[1].strip()
        launched = subprocess.run(["bash", "-c", command], env=self.env,
                                  text=True, capture_output=True, timeout=10)
        self.assertEqual(launched.returncode, 0, launched.stderr)
        self.assertIn("test-builder", command)

    def test_default_is_unique(self):
        for _ in range(2):
            result = self.run_script()
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(list(self.root.glob("generated-*"))), 2)
        self.assertEqual((self.source / "keep.txt").read_text(), "source survives")

    def test_missing_source_does_not_create_target(self):
        (self.source / "CMakeLists.txt").unlink()
        target = self.root / "not-created"
        self.assertNotEqual(self.run_script(GUEST_DIR=str(target)).returncode, 0)
        self.assertFalse(target.exists())

    def test_invalid_targets(self):
        for target in ("relative", "/mnt/c/build", str(self.source / "nested")):
            with self.subTest(target=target):
                self.assertNotEqual(self.run_script(GUEST_DIR=target).returncode, 0)
        self.assertFalse((self.source / "nested").exists())

    def test_symlink_target_rejected(self):
        target = self.root / "link"
        destination = self.root / "absent"
        target.symlink_to(destination)
        self.assertNotEqual(self.run_script(GUEST_DIR=str(target)).returncode, 0)
        self.assertTrue(target.is_symlink())
        self.assertFalse(destination.exists())

    def test_build_failure_retains_copy(self):
        target = self.root / "failed-build"
        result = self.run_script(GUEST_DIR=str(target), WSL_TEST_BUILD_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((target / "keep.txt").read_text(), "source survives")
        self.assertNotIn("BUILD_WSL_OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
