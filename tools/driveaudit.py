#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only audit of an exFAT PS4 library drive: which pkgs are whole and which are not.

    python3 tools/driveaudit.py /Volumes/MyDrive
    python3 tools/driveaudit.py --json /Volumes/MyDrive > audit.json
    python3 tools/driveaudit.py --explain /Volumes/MyDrive

The drive is only read: nothing on it is written, renamed or deleted, ever. Of each
*.pkg only the first 4 KiB (the CNT header) are read; pkg data is never decrypted and
large files are never read whole. The header layout is the one src/pkg.c uses:

    0x000  4 bytes   magic 0x7f "CNT"
    0x040 36 bytes   content id (EP0001-CUSA00000_00-XXXXXXXXXXXXXXXX), NUL-padded
    0x430  8 bytes   declared pkg size, big-endian

Exit code: 1 when any ERROR finding is seen, 2 when the root cannot be read, else 0.
"""
import argparse
import json
import os
import sys

HEADER_READ = 0x1000  # only the first 4 KiB of a pkg are ever read
CNT_MAGIC = b"\x7fCNT"
CONTENT_ID_OFF = 0x40
CONTENT_ID_LEN = 36
SIZE_OFF = 0x430  # declared pkg size, big-endian u64 (src/pkg.c)

TRANSFER_EXTS = {".part", ".tmp", ".dl", ".aria2"}
CLUTTER_DIRS = {".Trashes", ".Spotlight-V100", ".fseventsd"}

# Every check: severity, what it means and what to do. ERROR: the file cannot work as it
# is, or the drive state needs action. WARN: worth a look. INFO: a note, no action needed.
CHECKS = {
    "empty-pkg": ("ERROR",
                  "A 0-byte pkg: a copy that never started or was cut at the first byte. "
                  "It can never install; delete it and copy the pkg again."),
    "bad-magic": ("ERROR",
                  "The file's first bytes are not the CNT magic of a PS4 pkg. Either it is not "
                  "a pkg at all (renamed rar/zip/iso) or its header is damaged. The console "
                  "cannot use it; check what the file really is and copy it again if it is a pkg."),
    "short-header": ("ERROR",
                     "The file is smaller than a pkg header, so the header itself is cut. "
                     "The copy was truncated almost at the start; copy the pkg again."),
    "bad-content-id": ("ERROR",
                       "The content id in the header has characters a real one never uses. The "
                       "header is damaged; the console refuses such a pkg. Copy it again."),
    "bad-title-id": ("WARN",
                     "The title id is not four capital letters and five digits (CUSA00000), or "
                     "belongs to a system app (NPXS) or a PS5 game (PPSA). A PS4 game library "
                     "has no use for it."),
    "size-zero": ("ERROR",
                  "The header declares a pkg size of 0 in a file that has data: the header is "
                  "damaged. A tiny damaged add-on with this shape timed out during hardware testing (correlation, not proven causality). "
                  "Do not let the console's installer (BGFT) "
                  "install this file; copy it again from the source."),
    "size-short": ("ERROR",
                   "Fewer bytes on disk than the header declares: the download or copy was cut. "
                   "The pkg cannot install or run as it is; finish the copy or start it again."),
    "size-excess": ("WARN",
                    "More bytes on disk than the header declares. Some tools append padding; a "
                    "concatenation accident appends garbage. The pkg may still work, but if it "
                    "misbehaves copy it again."),
    "dup-content-id": ("WARN",
                       "The same content id in more than one file: exact duplicates, each one "
                       "holding up to a game's size. Keep one copy."),
    "dup-title-id": ("INFO",
                     "Two pkgs of one game with the same declared size but different content "
                     "ids: probably the same pkg dumped twice (a game, its update and its "
                     "add-ons sharing the title id is normal and is not reported). Compare "
                     "them and keep one."),
    "apple-double": ("INFO",
                     "A macOS metadata sidecar (._ file). Harmless but useless on the drive; "
                     "`dot_clean` on a Mac removes them."),
    "unfinished-transfer": ("WARN",
                            "An unfinished download or copy (.part/.tmp/.dl). It holds space "
                            "and is not a usable pkg; resume it or delete it."),
    "aria2-control": ("INFO",
                      "An aria2 control file: a download with aria2 was never finished or its "
                      "control file was left behind."),
    "macos-trashes": ("WARN",
                      "macOS's .Trashes: files deleted on a Mac stay here and keep their space. "
                      "Empty the trash with the drive connected to reclaim it."),
    "macos-clutter": ("INFO",
                      "A macOS bookkeeping folder (.Spotlight-V100, .fseventsd) or .DS_Store "
                      "file: small clutter the console ignores."),
    "unreadable": ("WARN",
                   "The file could not be opened or measured. exFAT corruption or a permission "
                   "problem; check the drive."),
}

SEVERITY_ORDER = {"ERROR": 0, "WARN": 1, "INFO": 2}


def human_size(n):
    """Bytes as a short readable size."""
    for unit in ("bytes", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n} bytes"


def content_id_valid(raw):
    """The 36 content-id bytes follow the rules src/pkg.c checks: only the characters a
    real content id uses, NUL padding only after at least 21 characters."""
    nul = False
    for i, c in enumerate(raw):
        if nul:
            if c != 0:
                return False
            continue
        if c == 0:
            if i < 21:
                return False
            nul = True
            continue
        if not ((ord("A") <= c <= ord("Z")) or (ord("a") <= c <= ord("z")) or (ord("0") <= c <= ord("9")) or c in (ord("-"), ord("_"))):
            return False
    return True


def title_id_shape(tid):
    """Four capital letters and five digits, and not a system (NPXS) or PS5 (PPSA) id:
    the same shape src/pkg.h's game_id_at asks for."""
    if len(tid) != 9:
        return "a title id is 9 characters"
    if not tid[:4].isalpha() or not tid[:4].isupper() or not tid[4:].isdigit():
        return "not four capital letters and five digits"
    if tid.startswith("NPXS"):
        return "NPXS is a system app, not a game"
    if tid.startswith("PPSA"):
        return "PPSA is a PS5 game, not a PS4 one"
    return None


def finding(check, path, detail):
    severity, explain = CHECKS[check]
    return {"severity": severity, "check": check, "path": path, "detail": detail, "explain": explain}


def parse_pkg_header(head, size):
    """What the first 4 KiB say. Returns (problems, info); problems is a list of
    (check, detail), info has content_id/title_id/declared when they could be read."""
    problems, info = [], {}
    if len(head) < len(CNT_MAGIC) or head[:4] != CNT_MAGIC:
        got = head[:4].hex() if head else "nothing"
        problems.append(("bad-magic", f"first bytes are {got}, not 7f434e54 (CNT)"))
        return problems, info
    if size < HEADER_READ:
        problems.append(("short-header", f"the file is {size} bytes, smaller than the 4 KiB pkg header"))
        return problems, info
    raw_cid = head[CONTENT_ID_OFF:CONTENT_ID_OFF + CONTENT_ID_LEN]
    if len(raw_cid) < CONTENT_ID_LEN or not content_id_valid(raw_cid):
        problems.append(("bad-content-id", "the content id has characters a real one never uses"))
    else:
        cid = raw_cid.split(b"\0")[0].decode("ascii")
        info["content_id"] = cid
        tid = cid[7:16]
        info["title_id"] = tid
        why = title_id_shape(tid)
        if why:
            problems.append(("bad-title-id", f"title id {tid!r}: {why}"))
    if len(head) < SIZE_OFF + 8:
        problems.append(("short-header", "the header ends before the pkg size field"))
        return problems, info
    declared = int.from_bytes(head[SIZE_OFF:SIZE_OFF + 8], "big")
    info["declared"] = declared
    if declared == 0:
        problems.append(("size-zero", f"the header declares a size of 0 in a file of {human_size(size)}"))
    elif size < declared:
        missing = declared - size
        problems.append(("size-short",
                         f"{human_size(size)} on disk of {human_size(declared)} declared "
                         f"({missing / declared * 100:.1f}% missing)"))
    elif size > declared:
        problems.append(("size-excess",
                         f"{human_size(size - declared)} beyond the declared {human_size(declared)}"))
    return problems, info


def audit(root):
    """Walk root (read-only) and return (findings, stats)."""
    findings = []
    stats = {"files": 0, "pkgs": 0, "dirs": 0, "bytes": 0}
    by_content_id, by_title_id = {}, {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        rel_dir = os.path.relpath(dirpath, root)
        for d in dirnames:
            stats["dirs"] += 1
            rel = os.path.normpath(os.path.join(rel_dir, d))
            if d in CLUTTER_DIRS:
                check = "macos-trashes" if d == ".Trashes" else "macos-clutter"
                findings.append(finding(check, rel + "/", f"macOS clutter folder {d}"))
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.normpath(os.path.join(rel_dir, name))
            try:
                size = os.path.getsize(path)
            except OSError as e:
                findings.append(finding("unreadable", rel, f"cannot measure it: {e.strerror or e}"))
                continue
            stats["files"] += 1
            stats["bytes"] += size
            if name.startswith("._"):
                findings.append(finding("apple-double", rel, f"macOS metadata sidecar of {name[2:]!r}"))
                continue  # a sidecar of a pkg is not a pkg: never parsed as one
            if name == ".DS_Store":
                findings.append(finding("macos-clutter", rel, "macOS .DS_Store file"))
                continue
            ext = os.path.splitext(name)[1].lower()
            if ext in TRANSFER_EXTS:
                check = "aria2-control" if ext == ".aria2" else "unfinished-transfer"
                findings.append(finding(check, rel, f"unfinished transfer artifact ({human_size(size)})"))
                continue
            if not name.lower().endswith(".pkg"):
                continue
            stats["pkgs"] += 1
            if size == 0:
                findings.append(finding("empty-pkg", rel, "the file is empty"))
                continue
            try:
                with open(path, "rb") as f:
                    head = f.read(HEADER_READ)  # only the header, never the data
            except OSError as e:
                findings.append(finding("unreadable", rel, f"cannot open it: {e.strerror or e}"))
                continue
            problems, info = parse_pkg_header(head, size)
            for check, detail in problems:
                findings.append(finding(check, rel, detail))
            if "content_id" in info:
                by_content_id.setdefault(info["content_id"], []).append(rel)
                if "title_id" in info and "declared" in info:
                    by_title_id.setdefault(info["title_id"], {})[info["content_id"]] = (rel, info["declared"])
    for cid, paths in sorted(by_content_id.items()):
        if len(paths) > 1:
            findings.append(finding("dup-content-id", ", ".join(paths),
                                    f"content id {cid} in {len(paths)} files"))
    for tid, cids in sorted(by_title_id.items()):
        # A game, its update and its add-ons share one title id: normal, not reported.
        # Report only two different content ids of one title with the same declared size:
        # probably the same pkg dumped twice. Exact copies share the content id and are
        # the duplicate finding above.
        sizes = {}
        for cid, (rel, declared) in sorted(cids.items()):
            sizes.setdefault(declared, []).append(rel)
        for declared, paths in sorted(sizes.items()):
            if len(paths) > 1:
                findings.append(finding("dup-title-id", ", ".join(paths),
                                        f"title id {tid}: {len(paths)} different content ids, "
                                        f"each declaring {human_size(declared)}"))
    findings.sort(key=lambda f: (SEVERITY_ORDER[f["severity"]], f["path"]))
    return findings, stats


def report_text(root, findings, stats, explain=False):
    out = []
    out.append(f"PS4Mount drive audit: {root}")
    out.append("Read-only: nothing on the drive is written, renamed or deleted.")
    out.append(f"Scanned {stats['files']} files ({human_size(stats['bytes'])}), "
               f"{stats['pkgs']} pkgs, {stats['dirs']} folders.")
    out.append("")
    counts = {"ERROR": 0, "WARN": 0, "INFO": 0}
    for f in findings:
        counts[f["severity"]] += 1
        out.append(f"{f['severity']:5}  {f['path']}")
        out.append(f"       {f['detail']}")
        out.append(f"       {f['explain']}")
    if not findings:
        out.append("No problems found.")
    else:
        out.append("")
        out.append(f"Result: {counts['ERROR']} error(s), {counts['WARN']} warning(s), {counts['INFO']} note(s).")
    if explain:
        out.append("")
        out.append("What the checks mean. ERROR: the file cannot work as it is, or the drive")
        out.append("needs action. WARN: worth a look. INFO: a note, no action needed.")
        out.append("")
        for name in sorted(CHECKS):
            severity, text = CHECKS[name]
            out.append(f"  {name} ({severity}): {text}")
    return "\n".join(out)


def report_json(root, findings, stats, explain=False):
    counts = {"ERROR": 0, "WARN": 0, "INFO": 0}
    for f in findings:
        counts[f["severity"]] += 1
    obj = {
        "root": root,
        "read_only": True,
        "files_scanned": stats["files"],
        "pkgs_scanned": stats["pkgs"],
        "dirs_scanned": stats["dirs"],
        "bytes_scanned": stats["bytes"],
        "counts": {"error": counts["ERROR"], "warn": counts["WARN"], "info": counts["INFO"]},
        "ok": counts["ERROR"] == 0,
        "findings": findings,
    }
    if explain:
        obj["checks"] = {name: {"severity": sev, "explain": text} for name, (sev, text) in sorted(CHECKS.items())}
    return json.dumps(obj, indent=2)


def main(argv=None):
    p = argparse.ArgumentParser(description="Read-only audit of an exFAT PS4 library drive.",
                                epilog="Nothing on the drive is written or deleted; only the first "
                                       "4 KiB of each pkg are read.")
    p.add_argument("root", help="the drive's root folder")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    p.add_argument("--explain", action="store_true", help="add what every check means and what to do")
    args = p.parse_args(argv)
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"driveaudit: {args.root} is not a readable folder", file=sys.stderr)
        return 2
    findings, stats = audit(root)
    if args.json:
        print(report_json(root, findings, stats, args.explain))
    else:
        print(report_text(root, findings, stats, args.explain))
    return 1 if any(f["severity"] == "ERROR" for f in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
