#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline read-only exFAT extent & interleaving mapper (P4N-23).

Parses exFAT boot sector (with backup fallback), active FAT, allocation bitmap,
and directory entry sets (0x85/0xC0/0xC1) using only the Python standard library.
Opens the target device or image strictly O_RDONLY and never writes to it.

Usage:
    python3 tools/exfatmap.py <device-or-image> <file-path>...
    python3 tools/exfatmap.py <device-or-image> <file-path>... --json out.json
    python3 tools/exfatmap.py <device-or-image> --auto-cusa23827 --json out.json
"""
import argparse
import array
import hashlib
import json
import math
import os
import statistics
import struct
import sys

EOT = 0x00
BITMAP_ENTRY = 0x81
UPCASE_ENTRY = 0x82
LABEL_ENTRY = 0x83
FILE_ENTRY = 0x85
STREAM_ENTRY = 0xC0
NAME_ENTRY = 0xC1
EOC = 0xFFFFFFFF
BAD_CLUSTER = 0xFFFFFFF7

MAX_DIR_BYTES = 256 * 1024 * 1024
MAX_DIR_DEPTH = 64

CUSA23827_PATCH_SIZE = 55208181760
CUSA23827_ADDON_LABELS = (
    "CODMW3CONTENT003",
    "CODMW3CONTENT004",
    "CODMW3CONTENT005",
    "CODMW3CONTENT006",
)


class ExFatError(Exception):
    """Fatal exFAT format, validation, or target-resolution error."""


def u16le(b, off):
    return int.from_bytes(b[off:off + 2], "little")


def u32le(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def u64le(b, off):
    return int.from_bytes(b[off:off + 8], "little")


RAW_IO_ALIGNMENT = 512
READ_WINDOW_CHUNK = 4 * 1024 * 1024


def read_at(fd, offset, length):
    if length == 0:
        return b""
    aligned_start = offset - (offset % RAW_IO_ALIGNMENT)
    end = offset + length
    aligned_end = end + ((-end) % RAW_IO_ALIGNMENT)
    out = bytearray()
    pos = aligned_start
    while pos < aligned_end:
        read_pos = pos - (pos % RAW_IO_ALIGNMENT)
        overlap = pos - read_pos
        want = min(READ_WINDOW_CHUNK, aligned_end - read_pos)
        chunk = os.pread(fd, want, read_pos)
        if len(chunk) <= overlap:
            break
        new_bytes = chunk[overlap:]
        out.extend(new_bytes)
        pos += len(new_bytes)
    skip = offset - aligned_start
    if len(out) < skip + length:
        raise ExFatError(f"unexpected EOF reading {length} bytes at offset {offset}")
    return bytes(out[skip:skip + length])


def boot_checksum(region_11_sectors):
    c = 0
    for i, x in enumerate(region_11_sectors):
        if i in (106, 107, 112):
            continue
        c = (((c & 1) * 0x80000000) + (c >> 1) + x) & 0xFFFFFFFF
    return c


def entry_set_checksum(entries):
    c = 0
    for i, x in enumerate(entries):
        if i in (2, 3):
            continue
        c = (((c & 1) * 0x8000) + (c >> 1) + x) & 0xFFFF
    return c


def _parse_boot_region(fd, base_sector, probe_bps=512):
    first = read_at(fd, base_sector * probe_bps, 512)
    if first[0:3] != b"\xeb\x76\x90":
        raise ExFatError("invalid exFAT jump boot signature")
    if first[3:11] != b"EXFAT   ":
        raise ExFatError("filesystem name is not 'EXFAT   '")
    if first[11:64] != bytes(53):
        raise ExFatError("non-zero MustBeZero boot sector region")
    if first[510:512] != b"\x55\xaa":
        raise ExFatError("missing 0x55AA boot signature")

    bytes_shift = first[108]
    spc_shift = first[109]
    number_of_fats = first[110]
    if not (9 <= bytes_shift <= 12):
        raise ExFatError(f"invalid BytesPerSectorShift: {bytes_shift}")
    bps = 1 << bytes_shift
    if not (0 <= spc_shift <= 25 - bytes_shift):
        raise ExFatError(f"invalid SectorsPerClusterShift: {spc_shift}")
    spc = 1 << spc_shift
    cluster_bytes = bps * spc
    if cluster_bytes > 32 * 1024 * 1024:
        raise ExFatError(f"cluster size exceeds 32 MiB: {cluster_bytes}")

    boot = read_at(fd, base_sector * bps, bps)
    partition_offset_sectors = u64le(boot, 64)
    volume_length_sectors = u64le(boot, 72)
    fat_offset = u32le(boot, 80)
    fat_length = u32le(boot, 84)
    cluster_heap_offset = u32le(boot, 88)
    cluster_count = u32le(boot, 92)
    root_cluster = u32le(boot, 96)
    volume_flags = u16le(boot, 106)

    if number_of_fats not in (1, 2):
        raise ExFatError(f"invalid NumberOfFats: {number_of_fats}")
    active_fat = volume_flags & 1
    if active_fat >= number_of_fats:
        raise ExFatError(f"active_fat {active_fat} >= NumberOfFats {number_of_fats}")
    if cluster_count == 0 or not (2 <= root_cluster <= cluster_count + 1):
        raise ExFatError(f"invalid RootCluster {root_cluster} for ClusterCount {cluster_count}")
    if fat_offset < 24:
        raise ExFatError(f"FatOffset {fat_offset} < 24")
    if fat_length * bps < (cluster_count + 2) * 4:
        raise ExFatError("FatLength too small for ClusterCount")
    if fat_offset + fat_length * number_of_fats > cluster_heap_offset:
        raise ExFatError("FAT overlaps ClusterHeapOffset")
    if cluster_heap_offset + cluster_count * spc > volume_length_sectors:
        raise ExFatError("ClusterHeap exceeds VolumeLength")

    region11 = read_at(fd, base_sector * bps, 11 * bps)
    expected_ck = boot_checksum(region11)
    ck_sector = read_at(fd, (base_sector + 11) * bps, bps)
    expected_bytes = struct.pack("<I", expected_ck) * (bps // 4)
    if ck_sector != expected_bytes:
        raise ExFatError(f"boot checksum mismatch (computed 0x{expected_ck:08x})")

    return {
        "bytes_per_sector": bps,
        "sectors_per_cluster": spc,
        "cluster_bytes": cluster_bytes,
        "partition_offset_sectors_boot": partition_offset_sectors,
        "volume_length_sectors": volume_length_sectors,
        "fat_offset_sectors": fat_offset,
        "fat_length_sectors": fat_length,
        "cluster_heap_offset_sectors": cluster_heap_offset,
        "cluster_heap_offset_bytes_volume_relative": cluster_heap_offset * bps,
        "cluster_count": cluster_count,
        "root_cluster": root_cluster,
        "volume_flags": volume_flags,
        "number_of_fats": number_of_fats,
        "active_fat": active_fat,
        "volume_dirty": bool(volume_flags & 2),
        "media_failure": bool(volume_flags & 4),
    }


class ExFatVolume:
    def __init__(self, image_path, partition_offset_bytes=0):
        self.image_path = image_path
        self.partition_offset_bytes = int(partition_offset_bytes)
        self.fd = os.open(image_path, os.O_RDONLY)
        self.warnings = []
        self.geometry = None
        self.fat_bytes = b""
        self.bitmap_bytes = None
        self.bitmap_length = 0
        try:
            self._load_geometry()
            self._load_fat()
            self._load_bitmap_from_root()
        except Exception:
            self.close()
            raise

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    def _load_geometry(self):
        main_err = None
        try:
            g = _parse_boot_region(self.fd, 0)
            g["boot_region_used"] = "main"
        except ExFatError as exc:
            main_err = exc
            # Probe sector size from backup region (512..4096)
            g = None
            for bps in (512, 1024, 2048, 4096):
                try:
                    cand = _parse_boot_region(self.fd, 12, probe_bps=bps)
                    if cand["bytes_per_sector"] == bps:
                        g = cand
                        break
                except ExFatError:
                    continue
            if g is None:
                raise ExFatError(f"main boot region invalid ({main_err}) and backup boot region invalid") from main_err
            g["boot_region_used"] = "backup"
            self.warnings.append(f"main boot region invalid ({main_err}); fell back to backup boot region")

        g["partition_offset_bytes"] = self.partition_offset_bytes
        if g["volume_dirty"]:
            self.warnings.append("VolumeDirty flag is set; filesystem metadata may be inconsistent")
        if g["media_failure"]:
            self.warnings.append("MediaFailure flag is set")
        self.geometry = g

    def _load_fat(self):
        g = self.geometry
        fat_sector = g["fat_offset_sectors"] + g["active_fat"] * g["fat_length_sectors"]
        fat_byte_count = (g["cluster_count"] + 2) * 4
        self.fat_bytes = read_at(self.fd, fat_sector * g["bytes_per_sector"], fat_byte_count)

    def fat_entry(self, cluster):
        return u32le(self.fat_bytes, cluster * 4)

    def cluster_volume_byte(self, cluster):
        g = self.geometry
        return (g["cluster_heap_offset_sectors"] + (cluster - 2) * g["sectors_per_cluster"]) * g["bytes_per_sector"]

    def follow_fat(self, start):
        if start == 0:
            return array.array("I")
        max_cluster = self.geometry["cluster_count"] + 1
        if not (2 <= start <= max_cluster):
            raise ExFatError(f"start cluster {start} outside heap [2..{max_cluster}]")
        out = array.array("I")
        seen = set()
        c = start
        while True:
            if c in seen:
                raise ExFatError(f"cycle in FAT chain at cluster {c}")
            if not (2 <= c <= max_cluster):
                raise ExFatError(f"cluster {c} outside heap [2..{max_cluster}]")
            seen.add(c)
            out.append(c)
            nxt = self.fat_entry(c)
            if nxt == EOC:
                return out
            if nxt == BAD_CLUSTER:
                raise ExFatError(f"bad cluster marker inside FAT chain at cluster {c}")
            if not (2 <= nxt <= max_cluster):
                raise ExFatError(f"invalid FAT next pointer 0x{nxt:08x} from cluster {c}")
            if len(out) > self.geometry["cluster_count"]:
                raise ExFatError("FAT chain longer than cluster heap")
            c = nxt

    def read_chain_bytes(self, first_cluster, no_fat_chain, length, max_bytes=None):
        if length == 0:
            return b""
        if max_bytes is not None and length > max_bytes:
            raise ExFatError(f"stream length {length} exceeds cap {max_bytes}")
        cbytes = self.geometry["cluster_bytes"]
        req = (length + cbytes - 1) // cbytes
        if no_fat_chain:
            max_c = self.geometry["cluster_count"] + 1
            if first_cluster < 2 or first_cluster + req - 1 > max_c:
                raise ExFatError("contiguous range exceeds cluster heap")
            return read_at(self.fd, self.cluster_volume_byte(first_cluster), length)
        chain = self.follow_fat(first_cluster)
        if len(chain) < req:
            raise ExFatError(f"FAT chain shorter than needed ({len(chain)} < {req} clusters)")
        out = bytearray()
        rem = length
        for c in chain[:req]:
            take = cbytes if rem > cbytes else rem
            out.extend(read_at(self.fd, self.cluster_volume_byte(c), take))
            rem -= take
        return bytes(out)

    def read_dir_clusters(self, first_cluster, no_fat_chain, data_length=None):
        cbytes = self.geometry["cluster_bytes"]
        if data_length is None:
            # Root directory: always follow FAT chain
            chain = self.follow_fat(first_cluster)
            total = len(chain) * cbytes
            if total > MAX_DIR_BYTES:
                raise ExFatError(f"root directory exceeds {MAX_DIR_BYTES} bytes")
            out = bytearray()
            for c in chain:
                out.extend(read_at(self.fd, self.cluster_volume_byte(c), cbytes))
            return bytes(out)
        if data_length > MAX_DIR_BYTES:
            raise ExFatError(f"directory exceeds {MAX_DIR_BYTES} bytes")
        return self.read_chain_bytes(first_cluster, no_fat_chain, data_length, max_bytes=MAX_DIR_BYTES)

    def _load_bitmap_from_root(self):
        root_bytes = self.read_dir_clusters(self.geometry["root_cluster"], False, None)
        active_fat = self.geometry["active_fat"]
        chosen = None
        fallback = None
        off = 0
        while off + 32 <= len(root_bytes):
            etype = root_bytes[off]
            if etype == EOT:
                break
            if etype == BITMAP_ENTRY:
                bflags = root_bytes[off + 1]
                fc = u32le(root_bytes, off + 20)
                dl = u64le(root_bytes, off + 24)
                if (bflags & 1) == active_fat:
                    chosen = (fc, dl)
                    break
                if fallback is None:
                    fallback = (fc, dl)
            off += 32
        if chosen is None:
            chosen = fallback
        if chosen is None:
            self.warnings.append("allocation bitmap entry (0x81) not found in root directory")
            return
        fc, dl = chosen
        if dl * 8 < self.geometry["cluster_count"]:
            raise ExFatError(f"allocation bitmap DataLength {dl} too small for ClusterCount {self.geometry['cluster_count']}")
        # Allocation bitmap may be followed through FAT (or contiguous if single cluster with FAT=EOC)
        self.bitmap_bytes = self.read_chain_bytes(fc, False, dl)
        self.bitmap_length = dl

    def cluster_allocated_in_bitmap(self, cluster):
        if self.bitmap_bytes is None:
            return None
        bit = cluster - 2
        return bool((self.bitmap_bytes[bit >> 3] >> (bit & 7)) & 1)

    def extents_all_allocated(self, extents):
        if self.bitmap_bytes is None:
            return None
        bm = self.bitmap_bytes
        for ext in extents:
            bstart = ext["physical_start_cluster"] - 2
            bend = bstart + ext["clusters"]
            while bstart < bend and (bstart & 7):
                if not ((bm[bstart >> 3] >> (bstart & 7)) & 1):
                    return False
                bstart += 1
            byte_start = bstart >> 3
            byte_end = bend >> 3
            if byte_end > byte_start:
                span = byte_end - byte_start
                if bm[byte_start:byte_end] != b"\xff" * span:
                    return False
                bstart = byte_end << 3
            while bstart < bend:
                if not ((bm[bstart >> 3] >> (bstart & 7)) & 1):
                    return False
                bstart += 1
        return True

    def parse_directory(self, buf, dir_path=""):
        entries_out = []
        off = 0
        n = len(buf)
        while off + 32 <= n:
            etype = buf[off]
            if etype == EOT:
                break
            if etype < 0x80:
                off += 32
                continue
            if etype in (BITMAP_ENTRY, UPCASE_ENTRY, LABEL_ENTRY):
                off += 32
                continue
            if etype != FILE_ENTRY:
                off += 32
                continue

            sec_count = buf[off + 1]
            set_len = (1 + sec_count) * 32
            if sec_count < 2 or off + set_len > n:
                raise ExFatError(f"truncated file entry set in '{dir_path or '/'}' at offset {off}")
            raw_set = buf[off:off + set_len]
            stored_ck = u16le(raw_set, 2)
            calc_ck = entry_set_checksum(raw_set)
            if stored_ck != calc_ck:
                raise ExFatError(
                    f"entry-set checksum mismatch in '{dir_path or '/'}' at offset {off} "
                    f"(stored 0x{stored_ck:04x}, computed 0x{calc_ck:04x})"
                )
            if raw_set[32] != STREAM_ENTRY:
                raise ExFatError(f"missing 0xC0 stream extension in '{dir_path or '/'}' at offset {off}")

            stream = raw_set[32:64]
            sflags = stream[1]
            alloc_possible = bool(sflags & 1)
            no_fat_chain = bool(sflags & 2)
            name_len = stream[3]
            valid_data_len = u64le(stream, 8)
            first_cluster = u32le(stream, 20)
            data_len = u64le(stream, 24)
            if not (1 <= name_len <= 255):
                raise ExFatError(f"invalid NameLength {name_len} in '{dir_path or '/'}'")
            req_name_entries = (name_len + 14) // 15
            if sec_count < 1 + req_name_entries:
                raise ExFatError(f"SecondaryCount {sec_count} too small for NameLength {name_len}")

            name_units = bytearray()
            for i in range(req_name_entries):
                p = 64 + i * 32
                if raw_set[p] != NAME_ENTRY:
                    raise ExFatError(f"expected 0xC1 name entry at slot {i} in '{dir_path or '/'}'")
                name_units.extend(raw_set[p + 2:p + 32])
            name = name_units[:name_len * 2].decode("utf-16-le", "replace")
            attrs = u16le(raw_set, 4)
            is_dir = bool(attrs & 0x10)
            full_path = f"{dir_path}/{name}" if dir_path else name
            if is_dir and valid_data_len != data_len:
                self.warnings.append(
                    f"directory '{full_path}' ValidDataLength ({valid_data_len}) != DataLength ({data_len})"
                )

            entries_out.append({
                "name": name,
                "path": full_path,
                "is_dir": is_dir,
                "attrs": attrs,
                "allocation_possible": alloc_possible,
                "no_fat_chain": no_fat_chain,
                "first_cluster": first_cluster,
                "data_length": data_len,
                "valid_data_length": valid_data_len,
            })
            off += set_len
        return entries_out

    def walk_files(self):
        root_fc = self.geometry["root_cluster"]
        visited = {root_fc}
        files = []

        def _walk(buf, dir_path, depth):
            if depth > MAX_DIR_DEPTH:
                raise ExFatError(f"directory depth exceeded {MAX_DIR_DEPTH} at '{dir_path}'")
            for entry in self.parse_directory(buf, dir_path):
                if entry["is_dir"]:
                    fc = entry["first_cluster"]
                    if fc == 0 or entry["data_length"] == 0:
                        continue
                    if fc in visited:
                        raise ExFatError(f"directory loop detected at '{entry['path']}' (cluster {fc})")
                    visited.add(fc)
                    sub_buf = self.read_dir_clusters(fc, entry["no_fat_chain"], entry["data_length"])
                    _walk(sub_buf, entry["path"], depth + 1)
                else:
                    files.append(entry)

        root_buf = self.read_dir_clusters(root_fc, False, None)
        _walk(root_buf, "", 0)
        return files

    def find_by_path(self, rel_path):
        norm = rel_path.strip("/")
        if not norm:
            raise ExFatError("empty file path")
        parts = [p for p in norm.split("/") if p]
        cur_buf = self.read_dir_clusters(self.geometry["root_cluster"], False, None)
        visited = {self.geometry["root_cluster"]}
        cur_path = ""
        for idx, part in enumerate(parts):
            entries = self.parse_directory(cur_buf, cur_path)
            want = part.casefold()
            hit = None
            for e in entries:
                if e["name"].casefold() == want:
                    hit = e
                    break
            if hit is None:
                raise ExFatError(f"path not found on exFAT volume: {rel_path}")
            is_last = idx == len(parts) - 1
            if is_last:
                if hit["is_dir"]:
                    raise ExFatError(f"path is a directory, not a file: {rel_path}")
                return hit
            if not hit["is_dir"]:
                raise ExFatError(f"intermediate component is not a directory: {hit['path']}")
            fc = hit["first_cluster"]
            if fc in visited:
                raise ExFatError(f"directory loop detected at '{hit['path']}'")
            visited.add(fc)
            cur_buf = self.read_dir_clusters(fc, hit["no_fat_chain"], hit["data_length"])
            cur_path = hit["path"]
        raise ExFatError(f"path not found: {rel_path}")

    def resolve_extents(self, entry):
        g = self.geometry
        cbytes = g["cluster_bytes"]
        dl = entry["data_length"]
        fc = entry["first_cluster"]
        nfc = entry["no_fat_chain"]
        required = (dl + cbytes - 1) // cbytes if dl > 0 else 0
        max_c = g["cluster_count"] + 1
        extra_clusters = 0

        if dl == 0:
            if fc != 0:
                self.warnings.append(f"zero-length file '{entry['path']}' has nonzero FirstCluster {fc}")
            return [], 0, 0, 0

        if fc == 0:
            raise ExFatError(f"nonzero file '{entry['path']}' has FirstCluster=0")

        extents = []
        if nfc:
            if fc < 2 or fc + required - 1 > max_c:
                raise ExFatError(f"NoFatChain file '{entry['path']}' exceeds cluster heap")
            chain_clusters = required
            vol_start = self.cluster_volume_byte(fc)
            vol_end = vol_start + required * cbytes
            extents.append({
                "logical_start_cluster": 0,
                "clusters": required,
                "physical_start_cluster": fc,
                "volume_byte_start": vol_start,
                "volume_byte_end_exclusive": vol_end,
                "disk_byte_start": self.partition_offset_bytes + vol_start,
                "disk_byte_end_exclusive": self.partition_offset_bytes + vol_end,
            })
        else:
            raw_chain = self.follow_fat(fc)
            chain_clusters = len(raw_chain)
            if chain_clusters < required:
                raise ExFatError(
                    f"FAT chain of '{entry['path']}' shorter than DataLength ({chain_clusters} < {required} clusters)"
                )
            if chain_clusters > required:
                extra_clusters = chain_clusters - required
                self.warnings.append(
                    f"FAT chain of '{entry['path']}' has {extra_clusters} extra cluster(s) past DataLength"
                )
            logical = raw_chain[:required]
            lstart = 0
            pstart = logical[0]
            prev = logical[0]
            run_len = 1
            for i in range(1, len(logical)):
                c = logical[i]
                if c == prev + 1:
                    run_len += 1
                else:
                    vs = self.cluster_volume_byte(pstart)
                    ve = vs + run_len * cbytes
                    extents.append({
                        "logical_start_cluster": lstart,
                        "clusters": run_len,
                        "physical_start_cluster": pstart,
                        "volume_byte_start": vs,
                        "volume_byte_end_exclusive": ve,
                        "disk_byte_start": self.partition_offset_bytes + vs,
                        "disk_byte_end_exclusive": self.partition_offset_bytes + ve,
                    })
                    lstart = i
                    pstart = c
                    run_len = 1
                prev = c
            vs = self.cluster_volume_byte(pstart)
            ve = vs + run_len * cbytes
            extents.append({
                "logical_start_cluster": lstart,
                "clusters": run_len,
                "physical_start_cluster": pstart,
                "volume_byte_start": vs,
                "volume_byte_end_exclusive": ve,
                "disk_byte_start": self.partition_offset_bytes + vs,
                "disk_byte_end_exclusive": self.partition_offset_bytes + ve,
            })
        return extents, required, chain_clusters, extra_clusters

    def read_first_4k_through_extents(self, extents, data_length):
        want = min(4096, data_length)
        if want == 0 or not extents:
            return b""
        out = bytearray()
        rem = want
        for ext in extents:
            ext_bytes = ext["volume_byte_end_exclusive"] - ext["volume_byte_start"]
            take = rem if rem < ext_bytes else ext_bytes
            out.extend(read_at(self.fd, ext["volume_byte_start"], take))
            rem -= take
            if rem == 0:
                break
        return bytes(out)

    def inspect_pkg_header(self, entry, extents):
        if not entry["name"].casefold().endswith(".pkg"):
            return None
        hdr = self.read_first_4k_through_extents(extents, entry["data_length"])
        h_sha = hashlib.sha256(hdr).hexdigest()
        if len(hdr) < 0x438 or hdr[0:4] != b"\x7fCNT":
            return {
                "valid_magic": False,
                "content_id": "",
                "title_id": "",
                "label": "",
                "declared_pkg_size": 0,
                "header_4k_sha256": h_sha,
            }
        cid_raw = hdr[0x40:0x64]
        cid = cid_raw.split(b"\x00", 1)[0].decode("ascii", "replace")
        tid = cid[7:16] if len(cid) >= 16 else ""
        label = cid[20:] if len(cid) >= 20 else ""
        declared = struct.unpack_from(">Q", hdr, 0x430)[0]
        return {
            "valid_magic": True,
            "content_id": cid,
            "title_id": tid,
            "label": label,
            "declared_pkg_size": declared,
            "header_4k_sha256": h_sha,
        }

    def analyze_entry(self, entry):
        cbytes = self.geometry["cluster_bytes"]
        extents, required, chain_clusters, extra_clusters = self.resolve_extents(entry)
        allocated_bytes = required * cbytes
        extent_count = len(extents)
        dl = entry["data_length"]

        if extent_count == 0:
            mean_ext = 0.0
            med_ext = 0.0
            largest_bytes = 0
            largest_share = 0.0
            phys_span = 0
        else:
            sizes = [e["clusters"] * cbytes for e in extents]
            mean_ext = sum(sizes) / extent_count
            med_ext = float(statistics.median(sizes))
            largest_bytes = max(sizes)
            largest_share = largest_bytes / allocated_bytes if allocated_bytes else 0.0
            phys_min = min(e["volume_byte_start"] for e in extents)
            phys_max = max(e["volume_byte_end_exclusive"] for e in extents)
            phys_span = phys_max - phys_min

        fwd_count = 0
        fwd_bytes = 0
        bwd_count = 0
        bwd_bytes = 0
        for i in range(extent_count - 1):
            cur = extents[i]
            nxt = extents[i + 1]
            cur_last_cluster = cur["physical_start_cluster"] + cur["clusters"] - 1
            signed_gap_clusters = nxt["physical_start_cluster"] - cur_last_cluster - 1
            signed_gap_bytes = signed_gap_clusters * cbytes
            if signed_gap_bytes > 0:
                fwd_count += 1
                fwd_bytes += signed_gap_bytes
            elif signed_gap_bytes < 0:
                bwd_count += 1
                bwd_bytes += -signed_gap_bytes

        gib = dl / (1024 ** 3) if dl > 0 else 0.0
        frags_per_gib = ((extent_count - 1) / gib) if (gib > 0 and extent_count > 1) else 0.0

        bm_ok = self.extents_all_allocated(extents)
        if bm_ok is False:
            self.warnings.append(f"file '{entry['path']}' uses cluster(s) not marked allocated in active bitmap")

        pkg = self.inspect_pkg_header(entry, extents)
        if pkg and pkg["valid_magic"] and pkg["declared_pkg_size"] != dl:
            self.warnings.append(
                f"PKG '{entry['path']}' declared size ({pkg['declared_pkg_size']}) != DataLength ({dl})"
            )

        return {
            "path": entry["path"],
            "data_length": dl,
            "valid_data_length": entry["valid_data_length"],
            "first_cluster": entry["first_cluster"],
            "no_fat_chain": entry["no_fat_chain"],
            "required_clusters": required,
            "chain_clusters": chain_clusters,
            "extra_clusters": extra_clusters,
            "allocated_bytes": allocated_bytes,
            "extent_count": extent_count,
            "contiguous": extent_count <= 1,
            "fragments_per_gib": round(frags_per_gib, 6),
            "mean_extent_bytes": round(mean_ext, 2),
            "median_extent_bytes": round(med_ext, 2),
            "largest_extent_bytes": largest_bytes,
            "largest_extent_share": round(largest_share, 6),
            "forward_gap_count": fwd_count,
            "forward_gap_bytes_total": fwd_bytes,
            "backward_jump_count": bwd_count,
            "backward_jump_bytes_total": bwd_bytes,
            "physical_span_bytes": phys_span,
            "bitmap_all_allocated": bm_ok,
            "pkg": pkg,
            "extents": extents,
        }


def _holes_of(sorted_extents):
    holes = []
    for i in range(len(sorted_extents) - 1):
        a_end = sorted_extents[i]["volume_byte_end_exclusive"]
        b_start = sorted_extents[i + 1]["volume_byte_start"]
        if b_start > a_end:
            holes.append((a_end, b_start))
    return holes


def _inside_holes(other_extents, holes):
    if not holes or not other_extents:
        return 0, 0
    ext_hits = 0
    byte_hits = 0
    for ext in other_extents:
        s = ext["volume_byte_start"]
        e = ext["volume_byte_end_exclusive"]
        hit_any = False
        for hs, he in holes:
            if he <= s:
                continue
            if hs >= e:
                break
            ov = min(e, he) - max(s, hs)
            if ov > 0:
                byte_hits += ov
                hit_any = True
        if hit_any:
            ext_hits += 1
    return ext_hits, byte_hits


def compute_pairwise(name_a, fa, name_b, fb):
    ea = sorted(fa["extents"], key=lambda x: x["volume_byte_start"])
    eb = sorted(fb["extents"], key=lambda x: x["volume_byte_start"])

    overlap = 0
    closest_gap = None
    i = 0
    j = 0
    while i < len(ea) and j < len(eb):
        as_, ae = ea[i]["volume_byte_start"], ea[i]["volume_byte_end_exclusive"]
        bs, be = eb[j]["volume_byte_start"], eb[j]["volume_byte_end_exclusive"]
        if ae <= bs:
            gap = bs - ae
            if closest_gap is None or gap < closest_gap:
                closest_gap = gap
            i += 1
        elif be <= as_:
            gap = as_ - be
            if closest_gap is None or gap < closest_gap:
                closest_gap = gap
            j += 1
        else:
            overlap += min(ae, be) - max(as_, bs)
            if ae < be:
                i += 1
            else:
                j += 1

    b_ext_in_a, b_bytes_in_a = _inside_holes(eb, _holes_of(ea))
    a_ext_in_b, a_bytes_in_b = _inside_holes(ea, _holes_of(eb))
    start_delta = (eb[0]["volume_byte_start"] - ea[0]["volume_byte_start"]) if (ea and eb) else None

    return {
        "a": name_a,
        "b": name_b,
        "overlap_bytes": overlap,
        "closest_positive_gap_bytes": closest_gap,
        "start_delta_bytes": start_delta,
        "b_extents_inside_a_holes": b_ext_in_a,
        "b_extent_bytes_inside_a_holes": b_bytes_in_a,
        "a_extents_inside_b_holes": a_ext_in_b,
        "a_extent_bytes_inside_b_holes": a_bytes_in_b,
        "interleaved": (b_bytes_in_a > 0) or (a_bytes_in_b > 0),
    }


def analyze_paths(image_path, rel_paths, partition_offset_bytes=0):
    with ExFatVolume(image_path, partition_offset_bytes=partition_offset_bytes) as vol:
        targets = {}
        order = []
        for p in rel_paths:
            entry = vol.find_by_path(p)
            key = entry["path"]
            if key in targets:
                continue
            targets[key] = vol.analyze_entry(entry)
            order.append(key)

        pairwise = []
        for i in range(len(order)):
            for j in range(i + 1, len(order)):
                pair = compute_pairwise(order[i], targets[order[i]], order[j], targets[order[j]])
                if pair["overlap_bytes"] > 0:
                    raise ExFatError(
                        f"fatal cluster overlap ({pair['overlap_bytes']} bytes) between '{order[i]}' and '{order[j]}'"
                    )
                pairwise.append(pair)

        return {
            "geometry": vol.geometry,
            "targets": targets,
            "pairwise": pairwise,
            "warnings": vol.warnings,
        }


def analyze_cusa23827_auto(image_path, partition_offset_bytes=0):
    with ExFatVolume(image_path, partition_offset_bytes=partition_offset_bytes) as vol:
        all_files = vol.walk_files()
        patch_cands = []
        addon_cands = {lbl: [] for lbl in CUSA23827_ADDON_LABELS}
        for f in all_files:
            if not f["name"].casefold().endswith(".pkg"):
                continue
            extents, _, _, _ = vol.resolve_extents(f)
            pkg = vol.inspect_pkg_header(f, extents)
            if not pkg or not pkg["valid_magic"]:
                continue
            if f["data_length"] == CUSA23827_PATCH_SIZE and pkg["title_id"] == "CUSA23827":
                patch_cands.append(f)
            if pkg["label"] in addon_cands:
                addon_cands[pkg["label"]].append(f)

        selected = {}
        ambiguous = []
        if len(patch_cands) == 1:
            selected["patch"] = vol.analyze_entry(patch_cands[0])
        else:
            ambiguous.append(f"patch candidates={len(patch_cands)}")
        for lbl in CUSA23827_ADDON_LABELS:
            if len(addon_cands[lbl]) == 1:
                selected[lbl] = vol.analyze_entry(addon_cands[lbl][0])
            else:
                ambiguous.append(f"{lbl} candidates={len(addon_cands[lbl])}")
        if ambiguous:
            raise ExFatError("ambiguous CUSA23827 target resolution: " + ", ".join(ambiguous))

        keys = list(selected.keys())
        pairwise = []
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                pair = compute_pairwise(keys[i], selected[keys[i]], keys[j], selected[keys[j]])
                if pair["overlap_bytes"] > 0:
                    raise ExFatError(
                        f"fatal cluster overlap ({pair['overlap_bytes']} bytes) between '{keys[i]}' and '{keys[j]}'"
                    )
                pairwise.append(pair)

        return {
            "geometry": vol.geometry,
            "targets": selected,
            "pairwise": pairwise,
            "warnings": vol.warnings,
        }


def format_human(report):
    g = report["geometry"]
    lines = [
        "== exFAT Geometry",
        f"  bytes_per_sector={g['bytes_per_sector']} sectors_per_cluster={g['sectors_per_cluster']} "
        f"cluster_bytes={g['cluster_bytes']} cluster_count={g['cluster_count']}",
        f"  cluster_heap_offset_sectors={g['cluster_heap_offset_sectors']} "
        f"partition_offset_bytes={g['partition_offset_bytes']} "
        f"fats={g['number_of_fats']} active_fat={g['active_fat']} "
        f"boot={g['boot_region_used']} dirty={g['volume_dirty']}",
    ]
    if report["warnings"]:
        lines.append("")
        lines.append("== Warnings")
        for w in report["warnings"]:
            lines.append(f"  [WARN] {w}")

    lines.append("")
    lines.append("== Targets")
    for name, t in report["targets"].items():
        lines.append(
            f"  {name}: size={t['data_length']} no_fat_chain={t['no_fat_chain']} "
            f"first_cluster={t['first_cluster']} extent_count={t['extent_count']} "
            f"contiguous={t['contiguous']} fragments_per_GiB={t['fragments_per_gib']:.3f} "
            f"largest_extent_share={t['largest_extent_share']:.4f} "
            f"forward_gaps={t['forward_gap_count']}({t['forward_gap_bytes_total']}B) "
            f"backward_jumps={t['backward_jump_count']}({t['backward_jump_bytes_total']}B) "
            f"bitmap_ok={t['bitmap_all_allocated']}"
        )
        if t["pkg"]:
            p = t["pkg"]
            lines.append(
                f"    pkg: valid={p['valid_magic']} cid={p['content_id']} "
                f"declared={p['declared_pkg_size']} sha256_4k={p['header_4k_sha256'][:16]}..."
            )
        for idx, ext in enumerate(t["extents"][:16]):
            lines.append(
                f"    ext[{idx}]: lcluster={ext['logical_start_cluster']} "
                f"pcluster={ext['physical_start_cluster']} clusters={ext['clusters']} "
                f"vol=[{ext['volume_byte_start']}..{ext['volume_byte_end_exclusive']})"
            )
        if len(t["extents"]) > 16:
            lines.append(f"    ... ({len(t['extents']) - 16} more extents)")

    if report["pairwise"]:
        lines.append("")
        lines.append("== Pairwise Interleaving")
        for p in report["pairwise"]:
            lines.append(
                f"  {p['a']} <-> {p['b']}: interleaved={p['interleaved']} "
                f"overlap_bytes={p['overlap_bytes']} closest_gap={p['closest_positive_gap_bytes']} "
                f"b_in_a_holes={p['b_extents_inside_a_holes']}({p['b_extent_bytes_inside_a_holes']}B) "
                f"a_in_b_holes={p['a_extents_inside_b_holes']}({p['a_extent_bytes_inside_b_holes']}B)"
            )
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Read-only offline exFAT extent, fragmentation, and interleaving mapper."
    )
    ap.add_argument("image", help="Raw device node (e.g. /dev/rdisk4s2) or exFAT image file")
    ap.add_argument("paths", nargs="*", help="Volume-relative file path(s) to map")
    ap.add_argument(
        "--partition-offset-bytes",
        type=int,
        default=0,
        help="Partition byte offset on the physical disk (added for disk_byte_* fields)",
    )
    ap.add_argument(
        "--auto-cusa23827",
        action="store_true",
        help="Walk volume and auto-select CUSA23827 patch.pkg + CODMW3CONTENT003..006",
    )
    ap.add_argument("--json", dest="json_out", help="Write full JSON report to PATH (or '-' for stdout)")
    args = ap.parse_args(argv)

    if not args.auto_cusa23827 and not args.paths:
        ap.error("provide at least one <file-path> or --auto-cusa23827")

    try:
        if args.auto_cusa23827:
            report = analyze_cusa23827_auto(args.image, partition_offset_bytes=args.partition_offset_bytes)
        else:
            report = analyze_paths(args.image, args.paths, partition_offset_bytes=args.partition_offset_bytes)
    except (ExFatError, OSError) as exc:
        sys.stderr.write(f"exfatmap: error: {exc}\n")
        return 1

    if args.json_out == "-":
        json.dump(report, sys.stdout, indent=2)
        sys.stdout.write("\n")
    elif args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
            fh.write("\n")
        sys.stdout.write(format_human(report))
    else:
        sys.stdout.write(format_human(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
