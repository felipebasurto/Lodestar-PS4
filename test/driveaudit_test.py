#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for tools/driveaudit.py: synthetic tiny pkg headers in temporary folders,
the CLI's human and JSON output, and its read-only promise. Run:

    python3 test/driveaudit_test.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import driveaudit  # noqa: E402

TOOL = os.path.join(ROOT, "tools", "driveaudit.py")

# A well-formed synthetic content id: 7 service chars, 9 title chars, _00-, 16 label chars.
GOOD_CID = b"EP0001-CUSA00000_00-AAAAAAAAAAAAAAAA"
PATCH_CID = b"EP0001-CUSA00000_00-BBBBBBBBBBBBBBBB"
DLC_CID = b"EP0001-CUSA00000_00-DLCPKG0000000001"


def make_pkg(path, size=0x4000, declared=None, content_id=GOOD_CID, magic=b"\x7fCNT"):
    """A synthetic pkg: a 4 KiB header and zeros up to `size`. Only the fields the
    auditor reads are set; the rest is not a real pkg and needs not be."""
    header = bytearray(0x1000)
    header[0:4] = magic
    cid = content_id[:36].ljust(36, b"\0")
    header[0x40:0x40 + 36] = cid
    struct.pack_into(">Q", header, 0x430, size if declared is None else declared)
    with open(path, "wb") as f:
        f.write(header)
        f.truncate(size)


def run_cli(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)


class AuditTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def pkg(self, name, **kw):
        path = os.path.join(self.root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True) if os.sep in name else None
        make_pkg(path, **kw)
        return path

    def audit(self):
        return driveaudit.audit(self.root)

    def checks(self, findings):
        return [f["check"] for f in findings]

    def test_clean_drive_has_no_findings(self):
        self.pkg("ps4/CUSA00000/app.pkg")
        self.pkg("ps4/CUSA00000/patch.pkg", content_id=PATCH_CID, size=0x2000)
        findings, stats = self.audit()
        self.assertEqual(findings, [])
        self.assertEqual(stats["pkgs"], 2)

    def test_bad_magic(self):
        self.pkg("notapkg.pkg", magic=b"PK\x03\x04")
        findings, _ = self.audit()
        self.assertIn("bad-magic", self.checks(findings))
        self.assertEqual(findings[0]["severity"], "ERROR")

    def test_empty_pkg(self):
        self.pkg("empty.pkg", size=0)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["empty-pkg"])

    def test_short_header(self):
        self.pkg("cut.pkg", size=0x200)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["short-header"])

    def test_bad_content_id(self):
        self.pkg("badcid.pkg", content_id=b"EP0001-CUSA00000_00-AAAAAA\x01AAAAAAAAA")
        findings, _ = self.audit()
        self.assertIn("bad-content-id", self.checks(findings))

    def test_content_id_nul_before_21(self):
        self.pkg("shortcid.pkg", content_id=b"EP0001-CUSA000\0")
        findings, _ = self.audit()
        self.assertIn("bad-content-id", self.checks(findings))

    def test_bad_title_id(self):
        self.pkg("sysapp.pkg", content_id=b"EP0001-NPXS00000_00-AAAAAAAAAAAAAAAA")
        findings, _ = self.audit()
        self.assertIn("bad-title-id", self.checks(findings))
        f = findings[self.checks(findings).index("bad-title-id")]
        self.assertEqual(f["severity"], "WARN")
        self.assertIn("system app", f["detail"])

    def test_zero_declared_size_is_damage(self):
        # A nonzero file whose header declares a size of 0: damaged, and such tiny
        # add-ons can stall the system's installer (BGFT).
        self.pkg("tinydlc.pkg", declared=0)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["size-zero"])
        self.assertEqual(findings[0]["severity"], "ERROR")
        self.assertIn("BGFT", findings[0]["explain"])

    def test_incomplete_pkg(self):
        self.pkg("cut.pkg", size=0x4000, declared=0x8000)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["size-short"])
        self.assertEqual(findings[0]["severity"], "ERROR")
        self.assertIn("50.0% missing", findings[0]["detail"])

    def test_excess_bytes(self):
        self.pkg("padded.pkg", size=0x5000, declared=0x4000)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["size-excess"])
        self.assertEqual(findings[0]["severity"], "WARN")

    def test_exact_size_is_clean(self):
        self.pkg("exact.pkg", size=0x4000, declared=0x4000)
        findings, _ = self.audit()
        self.assertEqual(findings, [])

    def test_duplicate_content_id(self):
        self.pkg("copy1/game.pkg")
        self.pkg("copy2/game.pkg")
        findings, _ = self.audit()
        self.assertIn("dup-content-id", self.checks(findings))
        self.assertNotIn("dup-title-id", self.checks(findings),
                         "the same game twice is the content-id finding, not a shared title")

    def test_shared_title_id_alone_is_not_reported(self):
        # A game, its update and its add-on share one title id: the normal library shape.
        self.pkg("ps4/CUSA00000/app.pkg")
        self.pkg("ps4/CUSA00000/patch.pkg", content_id=PATCH_CID, size=0x2000)
        self.pkg("ps4/CUSA00000/addcont/dlc.pkg", content_id=DLC_CID, size=0x1800)
        findings, _ = self.audit()
        self.assertEqual(findings, [])

    def test_same_title_and_size_is_a_note(self):
        # Same game, same declared size, different content ids: probably the same pkg twice.
        self.pkg("copy1/app.pkg", size=0x4000)
        self.pkg("copy2/app.pkg", content_id=b"EP0001-CUSA00000_00-CCCCCCCCCCCCCCCC", size=0x4000)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["dup-title-id"])
        self.assertEqual(findings[0]["severity"], "INFO")

    def test_appledouble_sidecar_is_not_parsed_as_pkg(self):
        self.pkg("real.pkg")
        # macOS writes ._real.pkg next to it; it must be a note, not a bad-magic error.
        with open(os.path.join(self.root, "._real.pkg"), "wb") as f:
            f.write(b"\x00\x05\x16\x07" + b"\0" * 100)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["apple-double"])
        self.assertEqual(findings[0]["severity"], "INFO")

    def test_unfinished_transfer_artifacts(self):
        for name in ("game.pkg.part", "game.pkg.tmp", "game.pkg.dl"):
            with open(os.path.join(self.root, name), "wb") as f:
                f.write(b"x" * 100)
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["unfinished-transfer"] * 3)
        self.assertTrue(all(f["severity"] == "WARN" for f in findings))

    def test_aria2_control_file(self):
        with open(os.path.join(self.root, "game.pkg.aria2"), "wb") as f:
            f.write(b"\x01")
        findings, _ = self.audit()
        self.assertEqual(self.checks(findings), ["aria2-control"])
        self.assertEqual(findings[0]["severity"], "INFO")

    def test_macos_clutter_dirs(self):
        for d in (".Trashes", ".Spotlight-V100", ".fseventsd"):
            os.makedirs(os.path.join(self.root, d))
        with open(os.path.join(self.root, ".DS_Store"), "wb") as f:
            f.write(b"\0" * 20)
        findings, _ = self.audit()
        checks = sorted(self.checks(findings))
        self.assertEqual(checks, ["macos-clutter", "macos-clutter", "macos-clutter", "macos-trashes"])
        sev = {f["check"]: f["severity"] for f in findings}
        self.assertEqual(sev["macos-trashes"], "WARN")
        self.assertEqual(sev["macos-clutter"], "INFO")

    def test_read_only_promise(self):
        # The audit changes nothing: same names, sizes and contents afterwards.
        self.pkg("ps4/CUSA00000/app.pkg")
        with open(os.path.join(self.root, "leftover.part"), "wb") as f:
            f.write(b"x" * 50)
        before = {}
        for dirpath, _, filenames in os.walk(self.root):
            for name in filenames:
                p = os.path.join(dirpath, name)
                with open(p, "rb") as fh:
                    before[p] = (os.path.getsize(p), fh.read())
        driveaudit.audit(self.root)
        run_cli(self.root)
        after = {}
        for dirpath, _, filenames in os.walk(self.root):
            for name in filenames:
                p = os.path.join(dirpath, name)
                with open(p, "rb") as fh:
                    after[p] = (os.path.getsize(p), fh.read())
        self.assertEqual(before, after)

    def test_only_the_header_is_read(self):
        # A pkg whose data region is a named pipe would hang a whole-file read; simpler:
        # the parse must need no more than what the first read brings.
        path = self.pkg("big.pkg", size=0x100000, declared=0x100000)
        with open(path, "rb") as f:
            head = f.read(0x1000)
        problems, info = driveaudit.parse_pkg_header(head, 0x100000)
        self.assertEqual(problems, [])
        self.assertEqual(info["content_id"], GOOD_CID.decode())
        self.assertEqual(info["title_id"], "CUSA00000")
        self.assertEqual(info["declared"], 0x100000)


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def pkg(self, name, **kw):
        make_pkg(os.path.join(self.root, name), **kw)

    def test_json_output_is_machine_readable(self):
        self.pkg("ok.pkg")
        self.pkg("cut.pkg", size=0x4000, declared=0x8000, content_id=PATCH_CID)
        r = run_cli("--json", self.root)
        self.assertEqual(r.returncode, 1)  # one ERROR finding
        obj = json.loads(r.stdout)
        self.assertEqual(obj["pkgs_scanned"], 2)
        self.assertFalse(obj["ok"])
        self.assertEqual(obj["counts"], {"error": 1, "warn": 0, "info": 0})
        checks = [f["check"] for f in obj["findings"]]
        self.assertEqual(checks, ["size-short"])
        f = obj["findings"][0]
        self.assertIn("severity", f)
        self.assertIn("explain", f)
        self.assertIn("path", f)

    def test_json_clean_drive_exits_zero(self):
        self.pkg("ok.pkg")
        r = run_cli("--json", self.root)
        self.assertEqual(r.returncode, 0)
        obj = json.loads(r.stdout)
        self.assertTrue(obj["ok"])
        self.assertEqual(obj["findings"], [])

    def test_human_output_explains_each_finding(self):
        self.pkg("cut.pkg", size=0x4000, declared=0x8000)
        r = run_cli(self.root)
        self.assertEqual(r.returncode, 1)
        self.assertIn("ERROR", r.stdout)
        self.assertIn("cut.pkg", r.stdout)
        self.assertIn("copy was cut", r.stdout)  # the plain-words explanation is in the report
        self.assertIn("Result: 1 error(s)", r.stdout)

    def test_human_clean_drive(self):
        self.pkg("ok.pkg")
        r = run_cli(self.root)
        self.assertEqual(r.returncode, 0)
        self.assertIn("No problems found.", r.stdout)
        self.assertIn("Read-only", r.stdout)

    def test_explain_lists_every_check(self):
        self.pkg("ok.pkg")
        r = run_cli("--explain", self.root)
        self.assertEqual(r.returncode, 0)
        for name in driveaudit.CHECKS:
            self.assertIn(name, r.stdout)
        self.assertIn("BGFT", r.stdout)

    def test_explain_json_adds_check_legend(self):
        self.pkg("ok.pkg")
        r = run_cli("--json", "--explain", self.root)
        obj = json.loads(r.stdout)
        self.assertIn("checks", obj)
        self.assertIn("size-zero", obj["checks"])
        self.assertEqual(obj["checks"]["size-zero"]["severity"], "ERROR")

    def test_missing_root_exits_2(self):
        r = run_cli(os.path.join(self.root, "no-such-place"))
        self.assertEqual(r.returncode, 2)

    def test_findings_sorted_errors_first(self):
        self.pkg("zzz_cut.pkg", size=0x4000, declared=0x8000)
        with open(os.path.join(self.root, "aaa_sidecar.tmp"), "wb") as f:
            f.write(b"x")
        r = run_cli("--json", self.root)
        checks = [f["check"] for f in json.loads(r.stdout)["findings"]]
        self.assertEqual(checks, ["size-short", "unfinished-transfer"])


if __name__ == "__main__":
    unittest.main()
