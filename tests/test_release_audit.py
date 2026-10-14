import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile


SPEC = importlib.util.spec_from_file_location("release_audit", Path(__file__).resolve().parents[1] / "tools/release_audit.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def private_email():
    return b"private" + b"@" + b"example.test"


def home_path():
    return b"/" + b"home" + b"/private/project"


class ContentTests(unittest.TestCase):
    def test_paths_and_email(self):
        self.assertEqual(audit.content_problems(home_path()), {"personal home path"})
        windows = "C:" + "\\" + "Users" + "\\" + "private" + "\\" + "project"
        self.assertIn("personal home path", audit.content_problems(windows.encode()))
        self.assertIn("personal home path", audit.content_problems(windows.encode("utf-16le"), binary=True))
        self.assertIn("personal home path", audit.content_problems(b"MZx" + home_path().decode().encode("utf-16le"), binary=True))
        self.assertEqual(audit.content_problems(private_email()), {"email address"})

    def test_noreply(self):
        email = b"12345+author" + b"@" + b"users.noreply.github.com"
        self.assertFalse(audit.content_problems(email))

    def test_credentials_without_leaking_values(self):
        token = b"ghp" + b"_" + b"a" * 36
        self.assertEqual(audit.content_problems(token), {"possible credential"})
        key = b"-----BEGIN " + b"OPENSSH PRIVATE KEY-----"
        self.assertIn("possible credential", audit.content_problems(key))

    def test_debug_record_and_nontext_source(self):
        self.assertIn("possible CodeView/PDB debug record", audit.content_problems(b"MZ-RSDS", binary=True))
        self.assertIn("non-text source file", audit.content_problems(b"MZ\0"))

    def test_private_locations(self):
        for name in ["PERSONAL.md", "docs/HANDOFF.md", ".env.production", "vendor-dlls/core.dll", "third_party/header.h", "captures/frame.json", "re/out/decomp.c", ".wine-game/user.reg", "build/ref.txt"]:
            with self.subTest(name=name):
                self.assertIsNotNone(audit.path_problem(name))
        for name in ["README.md", "captures/.gitkeep", ".env.example", "scenarios/fluid/probe_golden.json", "re/scripts/DecompileAtList.java"]:
            with self.subTest(name=name):
                self.assertIsNone(audit.path_problem(name))


class RepositoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        audit.git(self.root, "init", "-q")
        audit.git(self.root, "config", "commit.gpgsign", "false")
        audit.git(self.root, "config", "tag.gpgsign", "false")
        audit.git(self.root, "config", "user.name", "Test Author")
        audit.git(self.root, "config", "user.email", (b"author" + b"@" + b"users.noreply.github.com").decode())

    def commit(self):
        audit.git(self.root, "add", "-A")
        audit.git(self.root, "commit", "-qm", "Test fixture")

    def test_ignored_private_files_stay_out_but_forced_add_is_found(self):
        (self.root / ".gitignore").write_text("PERSONAL.md\n")
        (self.root / "PERSONAL.md").write_bytes(home_path())
        findings = set()
        self.assertEqual(audit.check_source(self.root, findings), 1)
        self.assertFalse(findings)
        audit.git(self.root, "add", "-f", "PERSONAL.md")
        audit.check_source(self.root, findings)
        self.assertIn(("PERSONAL.md", "private/generated file"), findings)
        self.assertIn(("PERSONAL.md", "personal home path"), findings)

    def test_old_content_and_author_remain_in_history(self):
        audit.git(self.root, "config", "user.email", private_email().decode())
        (self.root / "README.md").write_bytes(home_path())
        self.commit()
        (self.root / "README.md").write_text("Clean source\n")
        self.commit()
        findings = set()
        audit.check_source(self.root, findings)
        self.assertFalse(findings)
        audit.check_history(self.root, findings)
        self.assertIn(("history: README.md", "personal home path"), findings)
        self.assertIn(("history: commit/tag metadata", "email address"), findings)

    def test_deleted_forbidden_file_and_tag_message(self):
        (self.root / "README.md").write_text("Clean source\n")
        (self.root / "captured.bin").write_bytes(b"capture")
        self.commit()
        (self.root / "captured.bin").unlink()
        self.commit()
        audit.git(self.root, "tag", "-a", "test-tag", "-m", private_email().decode())
        findings = set()
        audit.check_history(self.root, findings)
        self.assertIn(("history: captured.bin", "binary, game data or diagnostic file"), findings)
        self.assertIn(("history: commit/tag metadata", "email address"), findings)

    def test_cli_nonzero_on_finding(self):
        (self.root / "README.md").write_bytes(home_path())
        result = subprocess.run(["python3", str(Path(audit.__file__)), "--history"], cwd=self.root, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(home_path(), result.stdout)


class ArtifactTests(unittest.TestCase):
    def test_expected_package_and_unexpected_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Shatter.zip"
            with zipfile.ZipFile(path, "w") as archive:
                for name in audit.PACKAGE_FILES:
                    archive.writestr("Shatter-1.0.0/" + name, b"MZfixture" if name.endswith(".dll") else b"Project text")
            findings = set()
            audit.check_artifact(path, findings)
            self.assertFalse(findings)
            with zipfile.ZipFile(path, "a") as archive:
                archive.writestr("../private.txt", home_path())
            audit.check_artifact(path, findings)
            self.assertIn((str(path), "unexpected ZIP contents"), findings)
            self.assertIn((str(path), "unsafe ZIP member path"), findings)
            self.assertTrue(any(problem == "personal home path" for _, problem in findings))

    def test_binary_utf16_and_debug_symbols(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ShatterSetup.exe"
            path.write_bytes(b"MZ-RSDS-" + home_path().decode().encode("utf-16le"))
            findings = set()
            audit.check_artifact(path, findings)
            self.assertIn((str(path), "possible CodeView/PDB debug record"), findings)
            self.assertIn((str(path), "personal home path"), findings)


if __name__ == "__main__":
    unittest.main()
