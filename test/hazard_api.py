#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Return-code checker for the user_id=0 class.

Watches sceUserService*, sceLncUtil* and sceAppInstUtil* calls in src/ and
panel-app/. A return that is discarded, or only printed, is a finding. So is a
foreground/initial user id that is allowed to stay 0 (or any id <= 0xFF).

  python3 -I test/hazard_api.py --policy     # CI: findings must match the baseline,
                                              # and the consent hard rule must be clean
  python3 -I test/hazard_api.py --list       # print findings
  python3 -I test/hazard_api.py --self-test  # checker against synthetic snippets
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "test", "hazard_api_baseline.txt")
WATCH_RE = re.compile(r"\b(sce(?:UserService|LncUtil|AppInstUtil)[A-Za-z0-9_]*)\s*\(")
DECL_RE = re.compile(
    r"^\s*(?:static\s+|extern\s+)*(?:int32_t|int64_t|uint32_t|int|void|ssize_t|size_t)\s+$"
)
PRINTF_NAMES = {"printf", "fprintf", "snprintf", "sprintf", "dprintf"}
RANGE_RE = re.compile(
    r"user_id_valid\s*\(|dbguard_user_ok\s*\(|"
    r"\buser(?:_id)?\s*(?:<=\s*0|<\s*1|==\s*0|!=\s*0|>\s*0\b|>\s*0x[Ff][Ff]\b|>\s*255\b)"
)
USER_GET = {"sceUserServiceGetForegroundUser", "sceUserServiceGetInitialUser"}


def blank_comments_and_strings(src):
    """Replace comments and string contents with spaces. Newlines stay put."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and nxt == "*":
            out.append(" ")
            out.append(" ")
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i + 1 < n:
                out.append(" ")
                out.append(" ")
                i += 2
        elif c in ('"', "'"):
            q = c
            out.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\" and i + 1 < n:
                    out.append(" ")
                    out.append(" ")
                    i += 2
                else:
                    out.append("\n" if src[i] == "\n" else " ")
                    i += 1
            if i < n:
                out.append(q)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(code, idx):
    return code.count("\n", 0, idx) + 1


def matching_paren(code, open_idx):
    depth = 0
    i = open_idx
    while i < len(code):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def statement_start(code, idx):
    depth = 0
    i = idx
    while i >= 0:
        c = code[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth = max(0, depth - 1)
        elif depth == 0 and c in ";{}":
            return i + 1
        i -= 1
    return 0


def container_name(code, stmt, name_start):
    depth = 0
    container = None
    i = stmt
    while i < name_start:
        if code[i] == "(":
            depth += 1
            if depth == 1:
                container = i
        elif code[i] == ")":
            depth = max(0, depth - 1)
        i += 1
    if depth <= 0 or container is None:
        return None
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", code[stmt:container])
    return m.group(1) if m else None


def classify_call(code, name_start, paren_open):
    """How the return value of the call at name_start is used."""
    stmt = statement_start(code, name_start)
    prefix = code[stmt:name_start]
    if DECL_RE.match(prefix):
        return "declaration"
    close = matching_paren(code, paren_open)
    if close is None:
        return "discarded"
    after = code[close + 1 : close + 1 + 32].lstrip()
    if after[:1] in "<>=!&|?":
        return "checked"
    if re.search(r"(?:==|!=|<=|>=)", prefix):
        return "checked"
    if re.search(r"(^|[^<>=!])=(?!=)", prefix):
        return "checked"
    if re.search(r"\b(if|while|for|return|switch)\b", prefix):
        return "checked"
    owner = container_name(code, stmt, name_start)
    if owner in PRINTF_NAMES:
        return "printf-only"
    if owner in ("if", "while", "for", "switch"):
        return "checked"
    if owner:
        return "argument"
    return "discarded"


def function_span(code, idx):
    """Best-effort brace span of the function containing idx."""
    brace = code.rfind("{", 0, idx)
    if brace < 0:
        return 0, len(code)
    depth = 0
    i = brace
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return brace, i + 1
        i += 1
    return brace, len(code)


def user_id_ranged(code, call_end, func_end):
    window = code[call_end:min(func_end, call_end + 700)]
    return RANGE_RE.search(window) is not None


def scan_text(path, src):
    code = blank_comments_and_strings(src)
    findings = []
    for m in WATCH_RE.finditer(code):
        name = m.group(1)
        kind = classify_call(code, m.start(1), m.end(1) - 1)
        if kind == "declaration":
            continue
        line = line_of(code, m.start(1))
        if kind in ("discarded", "printf-only", "argument"):
            findings.append((path, line, "%s return %s" % (name, kind)))
        if name in USER_GET:
            brace, end = function_span(code, m.start(1))
            body_before = code[brace:m.start(1)]
            zeroed = re.search(r"\buser\s*=\s*0\b", body_before) is not None
            if zeroed and kind != "checked":
                findings.append((path, line, "%s reads into a user id initialized to 0" % name))
            elif kind == "checked" and not user_id_ranged(code, matching_paren(code, m.end(1) - 1) or m.end(1), end):
                findings.append((path, line, "%s return is checked but user id 0 is still accepted" % name))
            elif zeroed and kind == "checked" and not user_id_ranged(code, matching_paren(code, m.end(1) - 1) or m.end(1), end):
                findings.append((path, line, "%s reads into a user id initialized to 0" % name))
    return findings


def source_files():
    files = []
    for folder in ("src", "panel-app"):
        d = os.path.join(ROOT, folder)
        for name in sorted(os.listdir(d)):
            if name.endswith((".c", ".h")):
                files.append(os.path.join(folder, name))
    return files


def scan_tree():
    found = []
    for rel in source_files():
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            found.extend(scan_text(rel, f.read()))
    return found


def format_finding(item):
    path, line, reason = item
    return "%s:%d: %s" % (path, line, reason)


def load_baseline():
    rows = []
    with open(BASELINE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                rows.append(line)
    return rows


def config_save_body(src):
    code = blank_comments_and_strings(src)
    m = re.search(r"\bconfig_save\s*\([^;{}]*\)\s*\{", code)
    if not m:
        return None
    close = matching_paren(code.replace("{", "(", 1) if False else code, m.end() - 1)
    # matching_paren counts parens, not braces. Do braces here.
    depth = 0
    i = m.end() - 1
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[m.end() : i]
        i += 1
    return None


def consent_violations(src):
    """Hard rule: first-run consent ends only after config.txt is renamed into place."""
    body = config_save_body(src)
    if body is None:
        return ["src/main.c: config_save() not found"]
    setup = body.find("setup_set(0)")
    rename = body.find("rename(")
    problems = []
    if setup < 0:
        problems.append("config_save never ends first-run consent with setup_set(0)")
    if rename < 0 or setup < rename:
        problems.append(
            "config_save calls setup_set(0) before rename() of the settings file "
            "(a failed or partial write would still open registration)"
        )
    if "return -1" not in body and "return (-1)" not in body:
        problems.append("config_save does not report a failed write")
    # Callers must use the return value. A bare config_save(); discards it.
    code = blank_comments_and_strings(src)
    for m in re.finditer(r"\bconfig_save\s*\(\s*\)\s*;", code):
        problems.append(
            "src/main.c:%d: config_save() return discarded" % line_of(code, m.start())
        )
    return problems


def policy():
    findings = [format_finding(f) for f in scan_tree()]
    baseline = load_baseline()
    ok = True
    if findings != baseline:
        ok = False
        print("sceUserService/sceLncUtil/sceAppInstUtil findings differ from test/hazard_api_baseline.txt")
        fs, bs = set(findings), set(baseline)
        for row in findings:
            if row not in bs:
                print("  new: " + row)
        for row in baseline:
            if row not in fs:
                print("  gone: " + row)
        if len(findings) != len(baseline):
            print("  count: found %d, baseline %d" % (len(findings), len(baseline)))
    main = open(os.path.join(ROOT, "src", "main.c"), encoding="utf-8").read()
    for problem in consent_violations(main):
        ok = False
        print("consent: " + problem)
    if ok:
        print("policy: system-API return baseline matches (%d known findings) and consent write is ordered" % len(findings))
    return 0 if ok else 1


def self_test():
    discarded = scan_text(
        "t.c",
        "void f(void) {\n"
        "    int32_t user = 0;\n"
        "    sceUserServiceGetForegroundUser(&user);\n"
        "    sceLncUtilLaunchApp(\"LODS00001\", 0, &user);\n"
        "}\n",
    )
    checked = scan_text(
        "t.c",
        "void f(void) {\n"
        "    int32_t user = -1;\n"
        "    int rc = sceUserServiceGetForegroundUser(&user);\n"
        "    if (rc != 0 || user <= 0 || user <= 0xFF)\n"
        "        return;\n"
        "    sceAppInstUtilInitialize();\n"
        "}\n",
    )
    declared = scan_text("t.c", "int sceUserServiceGetForegroundUser(int32_t *);\n")
    printed = scan_text(
        "t.c",
        "void f(void) {\n"
        "    printf(\"%x\\n\", (unsigned)sceLncUtilLaunchApp(\"T\", 0, 0));\n"
        "}\n",
    )
    initial = scan_text(
        "t.c",
        "int main(void) {\n"
        "    int32_t user = -1;\n"
        "    if (sceUserServiceGetInitialUser(&user) < 0)\n"
        "        return 1;\n"
        "    scePadOpen(user, 0, 0, 0);\n"
        "}\n",
    )
    bad = []
    kinds = [r for _, _, r in discarded]
    if not any("discarded" in r and "GetForegroundUser" in r for r in kinds):
        bad.append("discarded GetForegroundUser not flagged: %s" % kinds)
    if not any("initialized to 0" in r for r in kinds):
        bad.append("zero-init user not flagged: %s" % kinds)
    if not any("LaunchApp" in r and "discarded" in r for r in kinds):
        bad.append("discarded LaunchApp not flagged: %s" % kinds)
    if not any("AppInstUtilInitialize" in r and "discarded" in r for _, _, r in checked):
        bad.append("discarded Initialize inside the checked snippet was not flagged: %s" % checked)
    checked_user = [f for f in checked if "GetForegroundUser" in f[2]]
    if checked_user:
        bad.append("checked user path flagged: %s" % checked_user)
    if declared:
        bad.append("prototype flagged: %s" % declared)
    if not any("printf-only" in r for _, _, r in printed):
        bad.append("printf-only LaunchApp not flagged: %s" % printed)
    if not any("user id 0 is still accepted" in r for _, _, r in initial):
        bad.append("GetInitialUser < 0 without a range check not flagged: %s" % initial)
    consent_bad = consent_violations(
        "static void config_save(void) {\n"
        "    setup_set(0);\n"
        "    write_file(CONFIG_PATH, b.data, b.len, 0666);\n"
        "}\n"
        "void caller(void) { config_save(); }\n"
    )
    if not consent_bad:
        bad.append("consent rule missed setup_set before the write")
    consent_ok = consent_violations(
        "static int config_save(void) {\n"
        "    if (write_file(tmp, b.data, b.len, 0666) || rename(tmp, CONFIG_PATH)) {\n"
        "        return -1;\n"
        "    }\n"
        "    setup_set(0);\n"
        "    return 0;\n"
        "}\n"
        "void caller(void) { if (config_save()) return; }\n"
    )
    if consent_ok:
        bad.append("consent rule rejected a durable write: %s" % consent_ok)
    if bad:
        print("self-test failed:")
        for row in bad:
            print("  " + row)
        return 1
    print("self-test: checker flags discarded returns, user id 0, and a consent write that happens too early")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--policy" in argv:
        return policy()
    for row in scan_tree():
        print(format_finding(row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
