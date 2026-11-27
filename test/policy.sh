#!/bin/bash
# Rules the PS4Mount sources must keep, checked on every change: bash test/policy.sh
# Each rule comes from a problem seen on consoles (see the comments).
cd "$(dirname "$0")/.." || exit 1
fail=0
rule() { echo "::error::$1"; fail=1; }
SRC=(src/*.c src/*.h)
OTHER=() # every source but main.c
for f in src/*.c; do [ "$f" = src/main.c ] || OTHER+=("$f"); done

# The system databases are written by the system alone: rows written from outside made
# ShellCore flag app.db as corrupted (0.3.3), and read locks made its writes fail (0.3.4).
grep -nE 'sqlite3_(exec|blob_open|backup_init)\b' "${SRC[@]}" && rule "sqlite3 calls that can write: the system databases are read-only for PS4Mount"
grep -nE 'SQLITE_OPEN_(READWRITE|CREATE)' "${SRC[@]}" && rule "a database opened for writing"
grep -nE 'sqlite3_prepare' "${SRC[@]}" | grep -vE 'sqlite3_prepare(_v2)?\([^,]+, "SELECT ' && rule "SQL other than SELECT"
grep -n 'sqlite3_open' "${SRC[@]}" | grep -v 'mode=ro&nolock=1' && rule "every sqlite3_open must use mode=ro&nolock=1"
grep -nE '"/system_data/priv/mms/[^"]*"' "${SRC[@]}" | grep -vE 'APP_DB|db_mtime|static const char \*files|"/system_data/priv/mms/[a-z]+\.db(-journal)?"' &&
    rule "a path into the system database folder outside the read-only uses"
for f in "${SRC[@]}"; do
    awk '/write_file\(|open\([^)]*O_(WRONLY|RDWR)/ && /priv\/mms/ {print FILENAME":"NR": "$0; bad=1} END {exit bad}' "$f" ||
        rule "a write into /system_data/priv/mms"
done

# No shell, no other programs: the payload has no business running commands.
grep -nE '\b(system|popen|execl|execlp|execle|execv|execvp|execve)\s*\(' "${SRC[@]}" && rule "running commands"

# Unbounded string functions on data from files or the network.
grep -nE '\b(strcpy|strcat|sprintf|gets|vsprintf)\s*\(' "${SRC[@]}" && rule "unbounded string function (use snprintf/memcpy with a size)"

# Closing apps is only for moving pkgs back (close_open_games) and for the panel app on its
# own Back button (app_panel_close, which lists the panel app alone): nowhere else.
n=$(grep -c 'sceSystemServiceKillApp(' src/main.c)
[ "$n" -le 3 ] || rule "sceSystemServiceKillApp used outside close_open_games and app_panel_close ($n uses)"
awk '/^void app_panel_close/ {f=1} f && /open_apps\(/ && !/, 1\)/ {print FILENAME":"NR": "$0; bad=1} f && /^}/ {f=0} END {exit bad}' src/main.c ||
    rule "app_panel_close must list only the panel app (open_apps(..., 1))"
grep -nE 'sceSystemServiceKillApp|sceSystemServiceLaunchApp|sceLncUtil' "${OTHER[@]}" && rule "launching or closing apps outside main.c"

# The web panel changes state only on POST: every app_* call that changes something sits in
# a branch that checks for POST.
awk '/strn?cmp\(path/ {cond=$0} /app_(add|remove|restore|restored_forget|config_set|rescan|stop|panel_close)\(/ && cond !~ /post/ {print FILENAME":"NR": "$0; bad=1} END {exit bad}' src/web.c ||
    rule "a state change in the web panel reachable without POST"

# The page for older firmwares must stay without <dialog> and modern JavaScript.
grep -nE '</dialog>|showModal|fetch\(|=>|\blet |\bconst ' web/legacy.html && rule "legacy.html uses <dialog> or modern JavaScript"

# Every TR(...) text takes the same arguments in the same order: a translation with
# another % conversion would read the wrong argument.
python3 -I - "${SRC[@]}" <<'PY' || rule "a TR translation whose % conversions differ from the English text"
import re, sys
bad = 0
conv = re.compile(r'%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z|j|t)?[diouxXcspf%]')
for path in sys.argv[1:]:
    src = open(path, encoding="utf-8").read()
    for m in re.finditer(r'\bTR\(', src):
        depth, i = 1, m.end()
        while depth and i < len(src):
            c = src[i]
            if c == '"':
                i += 1
                while src[i] != '"':
                    i += 2 if src[i] == "\\" else 1
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            i += 1
        body = src[m.end():i - 1]
        if body.strip() == "...":
            continue  # the macro itself
        args = re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)(?:,|$)', body)
        texts = ["".join(re.findall(r'"((?:[^"\\]|\\.)*)"', a)) for a in args]
        convs = [conv.findall(t) for t in texts]
        if len(texts) < 2 or any(c != convs[0] for c in convs[1:]):
            line = src.count("\n", 0, m.start()) + 1
            print(f"{path}:{line}: {texts}")
            bad = 1
sys.exit(bad)
PY

# Notifications are in the console's language: a text given to notify() goes through TR()
# (a bare "%s" passes a text made with TR elsewhere).
grep -nE '\bnotify\("' src/main.c | grep -vE 'notify\("%s(: %s)?"' && rule "a notification without TR(): it would show in English on a Spanish console"

# The system cuts a pkg path at 95 bytes (96-byte buffers in app.json, the playgo records
# and the BGFT task): every path handed to it goes through pkg_url(), which hands a short
# link instead when the pkg's own path does not fit (shortpath.h).
awk '/^static const char \*pkg_url\(/ {inside=1} inside && /^\}/ {inside=0; next} /sys_path\(/ && !inside && !/^static const char \*sys_path\(/ {print FILENAME":"NR": "$0; bad=1} END {exit bad}' src/main.c ||
    rule "a pkg path handed to the system without pkg_url(): over 95 bytes it is cut"

# PANEL-10: The native PS4 title (LODS00001) must never terminate through libc exit/_exit/abort
# or by returning from main() (crt1 invokes libkernel:_exit -> ExceptionCode 12 / SIGSYS on PS4).
# It must always tear down cleanly (no detached threads) and exit via sceSystemServiceLoadExec.
PANEL_SRC=(panel-app/*.c panel-app/*.h)
grep -nE '\b(system|popen|execl|execlp|execle|execv|execvp|execve)\s*\(' "${PANEL_SRC[@]}" &&
    rule "panel-app: running external commands"
grep -nE '\b(strcpy|strcat|sprintf|gets|vsprintf)\s*\(' "${PANEL_SRC[@]}" &&
    rule "panel-app: unbounded string function (use snprintf/memcpy with a size)"
python3 -I - "${PANEL_SRC[@]}" <<'PY' || rule "panel-app violates PANEL-10 process lifecycle rules"
import re, sys

def strip_comments_and_strings(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                if src[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
        elif c in ('"', "'"):
            q = c
            out.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == '\\' and i + 1 < n:
                    i += 2
                else:
                    if src[i] == '\n':
                        out.append('\n')
                    i += 1
            if i < n:
                out.append(q)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)

bad = 0
forbid_calls = re.compile(r'\b(exit|_exit|_Exit|quick_exit|abort|pthread_detach)\s*\(')

for path in sys.argv[1:]:
    raw = open(path, encoding="utf-8").read()
    code = strip_comments_and_strings(raw)
    for m in forbid_calls.finditer(code):
        line = code.count('\n', 0, m.start()) + 1
        print(f"{path}:{line}: forbidden call '{m.group(1)}(' (PANEL-10)")
        bad = 1

    if path == "panel-app/main.c":
        if 'sceSystemServiceLoadExec("exit", NULL)' not in raw:
            print(f"{path}: missing sceSystemServiceLoadExec(\"exit\", NULL) (PANEL-10)")
            bad = 1
        m = re.search(r'\bint\s+main\s*\([^)]*\)\s*\{', code)
        if not m:
            print(f"{path}: main() definition not found")
            bad = 1
        else:
            depth, i = 1, m.end()
            while depth and i < len(code):
                if code[i] == '{':
                    depth += 1
                elif code[i] == '}':
                    depth -= 1
                i += 1
            body = code[m.end():i - 1]
            for rm in re.finditer(r'\breturn\b', body):
                line = code.count('\n', 0, m.end() + rm.start()) + 1
                print(f"{path}:{line}: return inside main() reaches crt1/_exit -> SIGSYS (PANEL-10)")
                bad = 1

    if path == "panel-app/art.c":
        if not re.search(r'#\s*define\s+STBI_ASSERT\b', raw):
            print(f"{path}: must define STBI_ASSERT before stb_image.h so abort/__assert_fail is not linked (PANEL-10)")
            bad = 1

sys.exit(bad)
PY

[ $fail = 0 ] && echo "policy: all rules hold"
exit $fail
