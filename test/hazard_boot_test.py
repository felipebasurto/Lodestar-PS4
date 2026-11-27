#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regression tests for the boot/registration hazard class.

Two kinds of check:

  required     must pass on this tree. The consent write (P4N-32) is one of them.
               It failed on the config_save() that was on main before this change
               (setup_set(0) ran before the write, and both callers discarded the
               return). The synthetic copy of that shape is still asserted below.

  expected-fail
               the safe behavior is NOT in this tree. The assertion is written so
               that it becomes true when the bug pattern is gone. An unexpected
               pass fails this script: drop the mark in the same change that fixes
               the bug. Each line names the open PR that already owns the fix,
               or "follow-up" when none of #15 #16 #17 #18 does.

Nothing here runs on a console. The predicates read src/main.c and panel-app/main.c.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hazard_api as hz

ROOT = hz.ROOT
MAIN = os.path.join(ROOT, "src", "main.c")
PANEL = os.path.join(ROOT, "panel-app", "main.c")


def body_from(code, opener):
    depth = 0
    i = opener
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[opener + 1 : i]
        i += 1
    raise AssertionError("unclosed brace at %d" % opener)


def body_of(code, name):
    m = re.search(r"\b%s\s*\([^;{}]*\)\s*\{" % re.escape(name), code)
    if not m:
        raise AssertionError("function %s not found" % name)
    return body_from(code, m.end() - 1)


def enclosing_function(code, idx):
    while True:
        depth = 0
        opener = None
        i = idx
        while i >= 0:
            if code[i] == "}":
                depth += 1
            elif code[i] == "{":
                if depth == 0:
                    opener = i
                    break
                depth -= 1
            i -= 1
        if opener is None:
            raise AssertionError("no enclosing function")
        sig = code[max(0, opener - 300) : opener]
        if re.search(r"\b(?:int|void)\s+[A-Za-z_][A-Za-z0-9_]*\s*\([^;{}]*\)\s*$", sig):
            return body_from(code, opener)
        idx = opener - 1


def safe_payload_user_checked(findings):
    """No discarded foreground-user read, and no user id left at 0, in the payload."""
    for path, _line, reason in findings:
        if path != "src/main.c":
            continue
        if "GetForegroundUser return discarded" in reason or "initialized to 0" in reason:
            return False
    return True


def safe_panel_rejects_user_zero(findings):
    for _path, _line, reason in findings:
        if "user id 0 is still accepted" in reason:
            return False
    return True


def safe_launch_result_branched(findings):
    for _path, _line, reason in findings:
        if "LaunchApp return" in reason:
            return False
    return True


def safe_failed_launch_can_retry(code):
    """The single auto-launch attempt must not be consumed before LaunchApp returns."""
    body = body_of(code, "panel_app_step")
    launch = body.find("sceLncUtilLaunchApp")
    if launch < 0:
        return True
    opened = body.find("opened = 1")
    if opened < 0:
        return True
    return opened > launch


def safe_initialize_checked(findings):
    for _path, _line, reason in findings:
        if "Initialize return discarded" in reason:
            return False
    return True


def safe_timeout_does_not_save(code):
    """The 120s first-run wait must not itself write the settings."""
    main = body_of(code, "main")
    i = main.find("SETUP_WAIT")
    if i < 0:
        return True
    return "config_save(" not in main[i : i + 500]


def safe_open_apps_fail_closed(code):
    """A failed process-table read is not 'no app is running'."""
    body = body_of(code, "open_apps")
    failed_len = re.search(r"\|\|\s*!len\)\s*return 0;", body) is not None
    failed_alloc = re.search(r"mem_free\(buf\);\s*return 0;", body) is not None
    return not failed_len and not failed_alloc


def safe_mask_counts_panel(code):
    """Staging /user/app must see LODS00001, not only games."""
    body = body_of(code, "mask_user_app")
    if "open_games(" in body:
        return False
    m = re.search(r"open_apps\s*\((?:[^()]|\([^()]*\))*\)", body)
    if not m:
        return False
    call = m.group(0)
    last = call.rsplit(",", 1)[-1]
    return "-1" in last or re.search(r"<\s*0", last) is not None


def safe_no_unmask_while_busy(code):
    """After the two-minute wait, do not unmount /user/app while AppInstallAll is still running."""
    body = body_of(code, "install_all")
    m = re.search(r"i\s*<\s*120", body)
    if not m:
        return True
    tail = body[m.start() :]
    if re.search(
        r"if\s*\(\s*masked\s*&&\s*!\s*(?:__atomic_load_n\s*\(\s*&g_install_busy|busy\b)",
        tail,
    ):
        return True
    if re.search(
        r"if\s*\(\s*masked\s*\)\s*unmask_user_app\s*\(|"
        r"if\s*\(\s*masked\s*&&\s*unmask_user_app\s*\(",
        tail,
    ):
        return False
    return "unmask_user_app(" not in tail


def safe_readd_stops(code):
    body = body_of(code, "readd_title")
    if "app_add(" not in body:
        return True
    return (
        re.search(r"if\s*\(\s*!rc\s*\)\s*\{[\s\S]*app_add\s*\(", body) is not None
        or re.search(r"if\s*\(\s*!rc\s*\)\s*app_add\s*\(", body) is not None
    )


def safe_uninstall_timeout_joined(code):
    """A timed-out uninstall must not keep running after the caller is told it failed.

    The earlier 'another uninstall is still running' return is before pthread_detach
    and is not this bug. Only a -2 after the thread has been detached counts.
    """
    body = body_of(code, "uninstall_title")
    if "pthread_detach(" not in body:
        return True
    after = body.split("pthread_detach(", 1)[1]
    return re.search(r"g_uninstall_busy[\s\S]{0,240}return\s+-2\s*;", after) is None


def safe_remove_uses_confirmed_read(code):
    """P4N-2: an unreadable app.db is not proof the title is absent."""
    body = body_of(code, "app_remove")
    return "db_is_registered(" not in body and "db_confirms_" in body


def safe_sandbox_failure_keeps_previous(code):
    body = body_of(code, "refresh_sandboxes")
    return (
        re.search(
            r"g_nsandboxes\s*=\s*0\s*;[\s\S]{0,180}if\s*\(\s*n\s*<=\s*0\s*\)\s*return\s*;",
            body,
        )
        is None
    )


def safe_registration_halts_on_corrupt_db(code):
    """install_all(ids, np) is skipped when the system database has been flagged."""
    m = re.search(r"install_all\s*\(\s*ids\s*,\s*np\s*\)", code)
    if not m:
        return True
    body = enclosing_function(code, m.start())
    rel = body.find("install_all")
    before = body[:rel]
    return re.search(r"if\s*\([^;{}]{0,120}(halt|dbguard|corrupt)", before) is not None


CASES = [
    (
        "H1",
        "payload launches and installs with a foreground user left at 0",
        "PR #15 (user id > 0xFF) and PR #16 (rejects 0, 0xFE and 0xFF; still accepts 1..0xFD)",
        safe_payload_user_checked,
        "findings",
    ),
    (
        "H1b",
        "panel app accepts user id 0 from sceUserServiceGetInitialUser",
        "not covered by PR #18 (it exits on rc < 0 only)",
        safe_panel_rejects_user_zero,
        "findings",
    ),
    (
        "H2",
        "the 120s first-run timeout writes the default settings and opens registration",
        "PR #15 and PR #16 (the timeout must not save). This tree still calls config_save there; P4N-32 only makes a failed write keep the gate closed",
        safe_timeout_does_not_save,
        "code",
    ),
    (
        "H3",
        "registration continues when app.db was flagged corrupt or rebuilt empty",
        "PR #16 halts. PR #15 only logs the app.db-corrupt marker and keeps registering",
        safe_registration_halts_on_corrupt_db,
        "code",
    ),
    (
        "H4a",
        "a failed process-table read looks like no app is running, so staging proceeds",
        "PR #15 (open_apps returns -1). PR #16 still returns 0",
        safe_open_apps_fail_closed,
        "code",
    ),
    (
        "H4b",
        "staging /user/app ignores a running LODS00001",
        "PR #16 (open_apps with panel < 0). PR #15 still calls open_games, which excludes the panel",
        safe_mask_counts_panel,
        "code",
    ),
    (
        "H5",
        "after 120s, /user/app is force-unmounted while AppInstallAll is still running",
        "neither PR #15 nor PR #16. Follow-up",
        safe_no_unmask_while_busy,
        "code",
    ),
    (
        "H6",
        "readd_title registers the game again even when the uninstall failed",
        "neither PR #15 nor PR #16 (PR #16 skips the readd only while its corrupt latch is set). Follow-up",
        safe_readd_stops,
        "code",
    ),
    (
        "H6b",
        "uninstall_title detaches the AppUnInstall thread and returns -2 while it can still finish",
        "follow-up. Not in PR #15-#18",
        safe_uninstall_timeout_joined,
        "code",
    ),
    (
        "H7",
        "app_remove treats db_is_registered == 0 as 'not installed' and skips the system uninstall",
        "follow-up. P4N-2 already forbids this; the call site still disagrees. Not fixed by PR #15-#18",
        safe_remove_uses_confirmed_read,
        "code",
    ),
    (
        "H8",
        "getfsstat failure clears the sandbox list, so a running game looks stopped",
        "follow-up. Not in PR #15-#18",
        safe_sandbox_failure_keeps_previous,
        "code",
    ),
    (
        "H9",
        "sceUserServiceInitialize and sceAppInstUtilInitialize returns are discarded",
        "PR #15 logs the payload UserService init only. AppInstUtilInitialize and the panel init stay discarded",
        safe_initialize_checked,
        "findings",
    ),
    (
        "H10",
        "sceLncUtilLaunchApp's return is only printed",
        "PR #15 stores the return and warns when it is negative. PR #16 still only prints it",
        safe_launch_result_branched,
        "findings",
    ),
    (
        "H10b",
        "the one automatic launch attempt is consumed before sceLncUtilLaunchApp returns",
        "follow-up. PR #15 sets opened=1 before the call, so a failed launch is not retried. PR #16 does the same once it decides to launch",
        safe_failed_launch_can_retry,
        "code",
    ),
]


def main():
    failed = 0
    if hz.self_test() != 0:
        failed += 1
    main_src = open(MAIN, encoding="utf-8").read()
    panel_src = open(PANEL, encoding="utf-8").read()
    code = hz.blank_comments_and_strings(main_src)
    findings = hz.scan_text("src/main.c", main_src) + hz.scan_text("panel-app/main.c", panel_src)

    consent = hz.consent_violations(main_src)
    if consent:
        failed += 1
        print("FAIL required: first-run consent is cleared before config.txt is renamed")
        for row in consent:
            print("  " + row)
    else:
        print("PASS required: config_save ends first-run consent only after rename() (P4N-32)")

    if hz.policy() != 0:
        failed += 1
        print("FAIL required: API-return baseline")
    else:
        print("PASS required: API-return baseline")

    # The shape that was on main must still be rejected, so this check cannot go quiet
    # just because the live function was fixed.
    old_shape = hz.consent_violations(
        "static void config_save(void) {\n"
        "    setup_set(0);\n"
        "    write_file(CONFIG_PATH, b.data, b.len, 0666);\n"
        "}\n"
        "void caller(void) { config_save(); }\n"
    )
    if not old_shape:
        failed += 1
        print("FAIL required: the pre-fix config_save shape is no longer detected")
    else:
        print("PASS required: the pre-fix config_save shape is still rejected (%d findings)" % len(old_shape))

    print("")
    print("expected-fail cases (each one is a bug still present on this tree):")
    for hid, title, owner, fn, kind in CASES:
        try:
            safe = fn(findings) if kind == "findings" else fn(code)
        except Exception as exc:
            failed += 1
            print("FAIL %s could not be evaluated: %s" % (hid, exc))
            continue
        if safe:
            failed += 1
            print("UNEXPECTED PASS %s: %s" % (hid, title))
            print("  the bug pattern is gone; remove this expected-fail. Owner was: %s" % owner)
        else:
            print("expected-fail: %s %s" % (hid, title))
            print("  owner: %s" % owner)

    print("")
    if failed:
        print("hazard_boot_test: %d problem(s)" % failed)
        return 1
    print("hazard_boot_test: required checks passed; expected-fail cases still fail on this tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
