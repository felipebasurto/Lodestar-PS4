#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for tools/exfatmap.py using synthetic in-memory/temp exFAT images (P4N-23).

Run:
    python3 -I test/exfatmap_test.py
"""
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exfatmap  # noqa: E402

TOOL = os.path.join(ROOT, "tools", "exfatmap.py")


class SyntheticExFat:
    """Minimal spec-conformant synthetic exFAT volume builder for host tests."""

    def __init__(
        self,
        bytes_shift=9,
        spc_shift=3,
        cluster_count=64,
        number_of_fats=1,
        active_fat=0,
        volume_flags=0,
    ):
        self.bps = 1 << bytes_shift
        self.spc = 1 << spc_shift
        self.cbytes = self.bps * self.spc
        self.bytes_shift = bytes_shift
        self.spc_shift = spc_shift
        self.cluster_count = cluster_count
        self.number_of_fats = number_of_fats
        self.active_fat = active_fat
        self.volume_flags = (volume_flags & ~1) | (active_fat & 1)

        self.fat_offset = 24
        self.fat_length = max(1, math.ceil(((cluster_count + 2) * 4) / self.bps))
        self.cluster_heap_offset = self.fat_offset + self.fat_length * self.number_of_fats
        self.volume_length = self.cluster_heap_offset + self.cluster_count * self.spc

        self.img = bytearray(self.volume_length * self.bps)
        self.fats = [[0] * (cluster_count + 2) for _ in range(number_of_fats)]
        for f in self.fats:
            f[0] = 0xFFFFFFF8
            f[1] = 0xFFFFFFFF

        # Cluster 2: allocation bitmap (active_fat 0), cluster 3: bitmap 1 if 2 FATs,
        # next cluster: root directory
        self.bitmap_clusters = [2] if number_of_fats == 1 else [2, 3]
        self.root_cluster = 3 if number_of_fats == 1 else 4
        self.bitmaps = [bytearray(math.ceil(cluster_count / 8)) for _ in range(number_of_fats)]

        for bc in self.bitmap_clusters:
            self.set_fat_chain([bc])
            self.mark_allocated([bc])
        self.set_fat_chain([self.root_cluster])
        self.mark_allocated([self.root_cluster])

        self.dir_entries = {self.root_cluster: bytearray()}
        for idx, bc in enumerate(self.bitmap_clusters):
            e = bytearray(32)
            e[0] = exfatmap.BITMAP_ENTRY
            e[1] = idx  # BitmapFlags bit 0 = fat index
            struct.pack_into("<I", e, 20, bc)
            struct.pack_into("<Q", e, 24, len(self.bitmaps[idx]))
            self.dir_entries[self.root_cluster].extend(e)

    def cluster_offset(self, c):
        return (self.cluster_heap_offset + (c - 2) * self.spc) * self.bps

    def set_fat_chain(self, clusters, fat_index=None, terminate=True):
        targets = range(self.number_of_fats) if fat_index is None else [fat_index]
        for fi in targets:
            for i in range(len(clusters) - 1):
                self.fats[fi][clusters[i]] = clusters[i + 1]
            if terminate and clusters:
                self.fats[fi][clusters[-1]] = exfatmap.EOC

    def mark_allocated(self, clusters, fat_index=None, allocated=True):
        targets = range(self.number_of_fats) if fat_index is None else [fat_index]
        for fi in targets:
            bm = self.bitmaps[fi]
            for c in clusters:
                bit = c - 2
                if allocated:
                    bm[bit >> 3] |= 1 << (bit & 7)
                else:
                    bm[bit >> 3] &= ~(1 << (bit & 7))

    def write_cluster(self, cluster, data):
        off = self.cluster_offset(cluster)
        padded = data[: self.cbytes].ljust(self.cbytes, b"\x00")
        self.img[off : off + self.cbytes] = padded

    def _make_entry_set(self, name, is_dir, first_cluster, data_length, no_fat_chain, valid_data_length=None):
        utf16 = name.encode("utf-16-le")
        n_name_entries = math.ceil(len(name) / 15)
        sec_count = 1 + n_name_entries
        buf = bytearray((1 + sec_count) * 32)
        buf[0] = exfatmap.FILE_ENTRY
        buf[1] = sec_count
        struct.pack_into("<H", buf, 4, 0x10 if is_dir else 0x20)

        buf[32] = exfatmap.STREAM_ENTRY
        buf[33] = 0x01 | (0x02 if no_fat_chain else 0x00)
        buf[35] = len(name)
        vdl = data_length if valid_data_length is None else valid_data_length
        struct.pack_into("<Q", buf, 32 + 8, vdl)
        struct.pack_into("<I", buf, 32 + 20, first_cluster)
        struct.pack_into("<Q", buf, 32 + 24, data_length)

        for i in range(n_name_entries):
            p = 64 + i * 32
            buf[p] = exfatmap.NAME_ENTRY
            chunk = utf16[i * 30 : (i + 1) * 30].ljust(30, b"\x00")
            buf[p + 2 : p + 32] = chunk

        ck = exfatmap.entry_set_checksum(buf)
        struct.pack_into("<H", buf, 2, ck)
        return buf

    def add_dir(self, parent_cluster, name, dir_cluster, no_fat_chain=True):
        self.mark_allocated([dir_cluster])
        if not no_fat_chain:
            self.set_fat_chain([dir_cluster])
        self.dir_entries[dir_cluster] = bytearray()
        eset = self._make_entry_set(name, True, dir_cluster, self.cbytes, no_fat_chain)
        self.dir_entries[parent_cluster].extend(eset)
        return dir_cluster

    def add_file(
        self,
        parent_cluster,
        name,
        clusters,
        data_length=None,
        no_fat_chain=False,
        content=b"",
        mark_bm=True,
        write_fat=True,
        fat_index=None,
        valid_data_length=None,
    ):
        dl = len(clusters) * self.cbytes if data_length is None else data_length
        fc = clusters[0] if clusters else 0
        if mark_bm and clusters:
            self.mark_allocated(clusters, fat_index=fat_index)
        if write_fat and clusters and not no_fat_chain:
            self.set_fat_chain(clusters, fat_index=fat_index)
        if content and clusters:
            pos = 0
            for c in clusters:
                if pos >= len(content):
                    break
                self.write_cluster(c, content[pos : pos + self.cbytes])
                pos += self.cbytes
        eset = self._make_entry_set(name, False, fc, dl, no_fat_chain, valid_data_length=valid_data_length)
        self.dir_entries[parent_cluster].extend(eset)

    def _write_boot_region(self, base_sector):
        boot = bytearray(self.bps)
        boot[0:3] = b"\xeb\x76\x90"
        boot[3:11] = b"EXFAT   "
        struct.pack_into("<Q", boot, 64, 2048)  # PartitionOffset cross-check field
        struct.pack_into("<Q", boot, 72, self.volume_length)
        struct.pack_into("<I", boot, 80, self.fat_offset)
        struct.pack_into("<I", boot, 84, self.fat_length)
        struct.pack_into("<I", boot, 88, self.cluster_heap_offset)
        struct.pack_into("<I", boot, 92, self.cluster_count)
        struct.pack_into("<I", boot, 96, self.root_cluster)
        struct.pack_into("<H", boot, 106, self.volume_flags)
        boot[108] = self.bytes_shift
        boot[109] = self.spc_shift
        boot[110] = self.number_of_fats
        boot[510:512] = b"\x55\xaa"

        region = bytearray(11 * self.bps)
        region[0 : self.bps] = boot
        ck = exfatmap.boot_checksum(region)
        ck_sec = struct.pack("<I", ck) * (self.bps // 4)
        off = base_sector * self.bps
        self.img[off : off + 11 * self.bps] = region
        self.img[off + 11 * self.bps : off + 12 * self.bps] = ck_sec

    def finalize(self):
        self._write_boot_region(0)
        self._write_boot_region(12)

        for fi in range(self.number_of_fats):
            fat_sec = self.fat_offset + fi * self.fat_length
            off = fat_sec * self.bps
            raw = struct.pack(f"<{len(self.fats[fi])}I", *self.fats[fi])
            self.img[off : off + len(raw)] = raw

        for fi, bc in enumerate(self.bitmap_clusters):
            self.write_cluster(bc, bytes(self.bitmaps[fi]))

        for dc, entries in self.dir_entries.items():
            self.write_cluster(dc, bytes(entries))

        return bytes(self.img)


def make_pkg_header(cid, declared_size):
    hdr = bytearray(4096)
    hdr[0:4] = b"\x7fCNT"
    hdr[0x40 : 0x40 + 36] = cid.encode("ascii")[:36].ljust(36, b"\x00")
    struct.pack_into(">Q", hdr, 0x430, declared_size)
    return bytes(hdr)


def run_cli(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)


class ExFatMapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write_img(self, name, raw):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(raw)
        return p

    def test_contiguous_nofatchain_and_fatchain_and_pkg_header(self):
        fs = SyntheticExFat(bytes_shift=9, spc_shift=3, cluster_count=64)  # 4 KiB clusters
        d_ps4 = fs.add_dir(fs.root_cluster, "ps4", 5)
        d_tid = fs.add_dir(d_ps4, "CUSA23827", 6)

        # File 1: NoFatChain=1 contiguous across clusters 10..13 (16 KiB).
        # Deliberately put bad/zero FAT values on clusters 10..13 to prove FAT is ignored!
        pkg_size = 4 * fs.cbytes
        hdr_patch = make_pkg_header("UP0002-CUSA23827_00-COREGAME00000001", pkg_size)
        fs.add_file(d_tid, "patch.pkg", [10, 11, 12, 13], data_length=pkg_size, no_fat_chain=True, content=hdr_patch)
        fs.fats[0][10] = exfatmap.BAD_CLUSTER
        fs.fats[0][11] = 0

        # File 2: NoFatChain=0 but still physically contiguous clusters 20..22 (12 KiB)
        hdr_app = make_pkg_header("UP0002-CUSA23827_00-CODMW3CONTENT003", 3 * fs.cbytes)
        fs.add_file(
            d_tid,
            "CODMW3CONTENT003_long_filename_test.pkg",
            [20, 21, 22],
            data_length=3 * fs.cbytes,
            no_fat_chain=False,
            content=hdr_app,
        )

        img_path = self.write_img("contig.img", fs.finalize())
        with open(img_path, "rb") as fh:
            before_sha = hashlib.sha256(fh.read()).hexdigest()

        rep = exfatmap.analyze_paths(
            img_path,
            ["/PS4/cusa23827/PATCH.PKG", "ps4/CUSA23827/CODMW3CONTENT003_long_filename_test.pkg"],
            partition_offset_bytes=1048576,
        )
        with open(img_path, "rb") as fh:
            after_sha = hashlib.sha256(fh.read()).hexdigest()
        self.assertEqual(before_sha, after_sha)

        t_patch = rep["targets"]["ps4/CUSA23827/patch.pkg"]
        self.assertTrue(t_patch["no_fat_chain"])
        self.assertEqual(t_patch["extent_count"], 1)
        self.assertTrue(t_patch["contiguous"])
        self.assertEqual(t_patch["fragments_per_gib"], 0.0)
        self.assertEqual(t_patch["largest_extent_share"], 1.0)
        self.assertEqual(t_patch["forward_gap_count"], 0)
        self.assertEqual(t_patch["backward_jump_count"], 0)
        self.assertTrue(t_patch["bitmap_all_allocated"])
        self.assertEqual(t_patch["pkg"]["valid_magic"], True)
        self.assertEqual(t_patch["pkg"]["title_id"], "CUSA23827")
        self.assertEqual(t_patch["pkg"]["declared_pkg_size"], pkg_size)
        self.assertEqual(
            t_patch["extents"][0]["disk_byte_start"],
            t_patch["extents"][0]["volume_byte_start"] + 1048576,
        )

        t_ac = rep["targets"]["ps4/CUSA23827/CODMW3CONTENT003_long_filename_test.pkg"]
        self.assertFalse(t_ac["no_fat_chain"])
        self.assertEqual(t_ac["extent_count"], 1)
        self.assertTrue(t_ac["contiguous"])
        self.assertEqual(t_ac["pkg"]["label"], "CODMW3CONTENT003")

        self.assertEqual(len(rep["pairwise"]), 1)
        pair = rep["pairwise"][0]
        self.assertFalse(pair["interleaved"])
        self.assertEqual(pair["overlap_bytes"], 0)
        self.assertEqual(pair["closest_positive_gap_bytes"], 6 * fs.cbytes)

    def test_fragmented_forward_gaps_backward_jumps_and_interleaving(self):
        fs = SyntheticExFat(bytes_shift=9, spc_shift=3, cluster_count=64)  # 4 KiB clusters
        # File A: logical clusters [10, 11] -> gap(4) -> [16, 17, 18] -> backward(13) -> [6, 7]
        # Sorted physical extents of A: [6..7], [10..11], [16..18]
        # Physical holes of A: [8..9] (2 clusters) and [12..15] (4 clusters)
        a_clusters = [10, 11, 16, 17, 18, 6, 7]
        fs.add_file(fs.root_cluster, "a.pkg", a_clusters, data_length=len(a_clusters) * fs.cbytes)

        # File B occupies [13, 14] inside A's hole [12..15] AND [25, 26] outside A's holes
        b_clusters = [13, 14, 25, 26]
        fs.add_file(fs.root_cluster, "b.pkg", b_clusters, data_length=len(b_clusters) * fs.cbytes)

        # File C is completely outside A and B at [30, 31]
        fs.add_file(fs.root_cluster, "c.pkg", [30, 31], data_length=2 * fs.cbytes)

        img_path = self.write_img("frag.img", fs.finalize())
        rep = exfatmap.analyze_paths(img_path, ["a.pkg", "b.pkg", "c.pkg"])

        ta = rep["targets"]["a.pkg"]
        self.assertEqual(ta["extent_count"], 3)
        self.assertFalse(ta["contiguous"])
        self.assertAlmostEqual(ta["largest_extent_share"], 3 / 7, places=5)
        self.assertEqual(ta["forward_gap_count"], 1)
        self.assertEqual(ta["forward_gap_bytes_total"], 4 * fs.cbytes)
        self.assertEqual(ta["backward_jump_count"], 1)
        self.assertEqual(ta["backward_jump_bytes_total"], 13 * fs.cbytes)
        expected_fpg = (3 - 1) / ((7 * fs.cbytes) / (1024 ** 3))
        self.assertAlmostEqual(ta["fragments_per_gib"], expected_fpg, places=1)

        pair_ab = [p for p in rep["pairwise"] if p["a"] == "a.pkg" and p["b"] == "b.pkg"][0]
        self.assertTrue(pair_ab["interleaved"])
        self.assertEqual(pair_ab["overlap_bytes"], 0)
        self.assertEqual(pair_ab["b_extents_inside_a_holes"], 1)
        self.assertEqual(pair_ab["b_extent_bytes_inside_a_holes"], 2 * fs.cbytes)
        # A's extent [16..18] sits inside B's hole [15..24]
        self.assertEqual(pair_ab["a_extents_inside_b_holes"], 1)
        self.assertEqual(pair_ab["a_extent_bytes_inside_b_holes"], 3 * fs.cbytes)
        self.assertEqual(pair_ab["closest_positive_gap_bytes"], 1 * fs.cbytes)

        pair_ac = [p for p in rep["pairwise"] if p["a"] == "a.pkg" and p["b"] == "c.pkg"][0]
        self.assertFalse(pair_ac["interleaved"])
        self.assertEqual(pair_ac["b_extents_inside_a_holes"], 0)

    def test_active_fat_1_and_backup_boot_fallback_and_dirty_warning(self):
        fs = SyntheticExFat(
            bytes_shift=9,
            spc_shift=3,
            cluster_count=64,
            number_of_fats=2,
            active_fat=1,
            volume_flags=0x0002,  # VolumeDirty
        )
        # Put valid chain ONLY in FAT 1; poison FAT 0 to prove active_fat=1 is used
        fs.add_file(fs.root_cluster, "fat1.bin", [10, 12], fat_index=1)
        fs.fats[0][10] = exfatmap.BAD_CLUSTER
        raw = bytearray(fs.finalize())
        # Corrupt main boot checksum sector (sector 11) so backup region (sectors 12..23) is used
        raw[11 * fs.bps] ^= 0xFF

        img_path = self.write_img("backup.img", bytes(raw))
        rep = exfatmap.analyze_paths(img_path, ["fat1.bin"])
        self.assertEqual(rep["geometry"]["boot_region_used"], "backup")
        self.assertEqual(rep["geometry"]["active_fat"], 1)
        self.assertTrue(rep["geometry"]["volume_dirty"])
        self.assertTrue(any("backup boot region" in w for w in rep["warnings"]))
        self.assertTrue(any("VolumeDirty" in w for w in rep["warnings"]))
        self.assertEqual(rep["targets"]["fat1.bin"]["extent_count"], 2)

    def test_validation_errors_cycle_truncation_overlap_and_bitmap_warning(self):
        # 1. FAT cycle
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "cyc.bin", [10, 11], write_fat=False)
        fs.fats[0][10] = 11
        fs.fats[0][11] = 10
        p = self.write_img("cycle.img", fs.finalize())
        with self.assertRaisesRegex(exfatmap.ExFatError, "cycle"):
            exfatmap.analyze_paths(p, ["cyc.bin"])

        # 2. Truncated chain
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "short.bin", [10, 11], data_length=3 * fs.cbytes)
        p = self.write_img("short.img", fs.finalize())
        with self.assertRaisesRegex(exfatmap.ExFatError, "shorter"):
            exfatmap.analyze_paths(p, ["short.bin"])

        # 3. Cluster overlap between two live files
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "f1.bin", [10, 11])
        fs.add_file(fs.root_cluster, "f2.bin", [11, 12])
        p = self.write_img("overlap.img", fs.finalize())
        with self.assertRaisesRegex(exfatmap.ExFatError, "overlap"):
            exfatmap.analyze_paths(p, ["f1.bin", "f2.bin"])

        # 4. Unallocated cluster in bitmap + extra FAT clusters -> warnings, not crash
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "warn.bin", [10, 11, 12], data_length=2 * fs.cbytes, mark_bm=False)
        p = self.write_img("warn.img", fs.finalize())
        rep = exfatmap.analyze_paths(p, ["warn.bin"])
        t = rep["targets"]["warn.bin"]
        self.assertFalse(t["bitmap_all_allocated"])
        self.assertEqual(t["extra_clusters"], 1)
        self.assertTrue(any("not marked allocated" in w for w in rep["warnings"]))
        self.assertTrue(any("extra cluster" in w for w in rep["warnings"]))

        # 5. Bad cluster marker inside FAT chain
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "bad.bin", [10, 11], write_fat=False)
        fs.fats[0][10] = exfatmap.BAD_CLUSTER
        p = self.write_img("badcluster.img", fs.finalize())
        with self.assertRaisesRegex(exfatmap.ExFatError, "bad cluster"):
            exfatmap.analyze_paths(p, ["bad.bin"])

        # 6. Entry-set checksum mismatch
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "ck.bin", [10])
        raw = bytearray(fs.finalize())
        # Corrupt the first file entry-set checksum in root directory (after 1 bitmap entry = +32 bytes)
        raw[fs.cluster_offset(fs.root_cluster) + 32 + 2] ^= 0xFF
        p = self.write_img("badck.img", bytes(raw))
        with self.assertRaisesRegex(exfatmap.ExFatError, "checksum"):
            exfatmap.analyze_paths(p, ["ck.bin"])

    def test_cli_human_and_json_output(self):
        fs = SyntheticExFat(cluster_count=32)
        fs.add_file(fs.root_cluster, "x.pkg", [10, 12])
        fs.add_file(fs.root_cluster, "y.pkg", [11])
        img_path = self.write_img("cli.img", fs.finalize())

        r_human = run_cli(img_path, "x.pkg", "y.pkg")
        self.assertEqual(r_human.returncode, 0, r_human.stderr)
        self.assertIn("extent_count=2", r_human.stdout)
        self.assertIn("interleaved=True", r_human.stdout)

        r_json = run_cli(img_path, "x.pkg", "y.pkg", "--json", "-")
        self.assertEqual(r_json.returncode, 0, r_json.stderr)
        doc = json.loads(r_json.stdout)
        self.assertEqual(doc["targets"]["x.pkg"]["extent_count"], 2)
        self.assertTrue(doc["pairwise"][0]["interleaved"])

        r_miss = run_cli(img_path, "missing.pkg")
        self.assertNotEqual(r_miss.returncode, 0)


if __name__ == "__main__":
    unittest.main()
