"""Check that the guard inspects actual index/history contents and redacts findings."""

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("check_push", Path(__file__).resolve().parents[1] / "check_push.py")
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class PushGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        guard.git(self.root, "init", "-q")
        guard.git(self.root, "config", "core.hooksPath", str(self.root / "no-hooks"))
        # Construct a synthetic pattern; no real credentials are used in tests.
        self.token = "gh" + "p_" + "Ab12" * 9

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_clean_files_and_template_are_accepted(self):
        self.write("README.md", "A clean research checkout.\n")
        self.write(".env.example", "API_KEY=<your own key>\n")
        self.assertEqual(guard.audit(self.root)[1], [])

    def test_staged_content_is_checked_even_if_worktree_is_clean(self):
        self.write("config.py", self.token)
        guard.git(self.root, "add", "config.py")
        self.write("config.py", "# secret removed locally\n")
        self.assertEqual(guard.audit(self.root)[1], [])
        errors = guard.audit(self.root, staged=True)[1]
        self.assertTrue(any("GitHub token" in e for e in errors))
        self.assertNotIn(self.token, "\n".join(errors))

    def test_untracked_secret_and_local_key_copy_are_detected(self):
        value = "Test" + "LocalCredential123456789"
        self.write(".gitignore", "KEYS.txt\n")
        self.write("data_sources/massive/KEYS.txt", "REST API\nOPTIONS/STOCKS\n" + value)
        self.write("notes.md", "Accidental copy: " + value)
        summary, errors = guard.audit(self.root)
        self.assertEqual(summary["local_credentials_compared"], 1)
        self.assertTrue(any("local Massive credential" in e for e in errors))
        self.assertNotIn(value, "\n".join(errors))

    def test_ignored_but_tracked_private_file_is_rejected(self):
        self.write(".gitignore", ".env.local\n")
        self.write(".env.local", "PRIVATE_CONFIGURATION=yes\n")
        guard.git(self.root, "add", "-f", ".env.local")
        for staged in (False, True):
            self.assertTrue(any("private configuration" in e for e in guard.audit(self.root, staged)[1]))

    def test_deleted_historical_secret_still_blocks(self):
        self.write("old.txt", self.token)
        guard.git(self.root, "add", "old.txt")
        guard.git(self.root, "-c", "user.name=Guard Test", "-c", "user.email=test@example.invalid",
                  "-c", "commit.gpgsign=false", "commit", "-qm", "Test fixture")
        guard.git(self.root, "rm", "old.txt")
        errors = guard.audit(self.root)[1]
        self.assertTrue(any(e.startswith("history:") and "GitHub token" in e for e in errors))

    def test_commit_messages_are_scanned(self):
        guard.git(self.root, "-c", "user.name=Guard Test", "-c", "user.email=test@example.invalid",
                  "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", self.token)
        self.assertTrue(any("history:commit:" in e for e in guard.audit(self.root)[1]))

    def test_large_files_are_blocked_before_reading(self):
        self.write("large.csv", "x" * 100)
        with patch.object(guard, "MAX_BYTES", 50):
            entries = list(guard.candidate_files(self.root, False))
            self.assertEqual(entries[0][2:], (100, b""))
            self.assertTrue(any("exceeds" in e for e in guard.audit(self.root)[1]))

    def test_conflicts_and_external_symlinks_are_blocked(self):
        self.write("source.py", "<" * 7 + " HEAD\n")
        (self.root / "external").symlink_to("../outside-repository")
        errors = guard.audit(self.root)[1]
        self.assertTrue(any("conflict marker" in e for e in errors))
        self.assertTrue(any("symlink" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
