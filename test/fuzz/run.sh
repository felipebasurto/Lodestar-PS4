#!/bin/bash
# Builds the fuzz targets with libFuzzer (clang) and runs each for SECONDS (default 60):
#   bash test/fuzz/run.sh [SECONDS] [target...]
# Crashes are kept in build/fuzz/<target>/ (crash-*, leak-*, timeout-*).
set -e
cd "$(dirname "$0")/../.."
SECONDS_EACH=${1:-60}
shift || true
TARGETS=${*:-"sfo pkg origin http rar5"}
B=build/fuzz
mkdir -p "$B"
cc_can_fuzz() {
    printf '%s\n' 'int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size) { return 0; }' > "$B/fuzz_cc_check.c"
    "$1" -fsanitize="$2" -o "$B/fuzz_cc_check" "$B/fuzz_cc_check.c" >/dev/null 2>&1 &&
        "$B/fuzz_cc_check" -runs=1 >/dev/null 2>&1
}
try_cc() {
    for s in fuzzer,address,undefined fuzzer,undefined; do
        if cc_can_fuzz "$1" "$s"; then
            SANITIZERS=$s
            return 0
        fi
    done
    return 1
}
if [ -z "${CC:-}" ]; then
    for c in clang /opt/homebrew/opt/llvm*/bin/clang /usr/local/opt/llvm*/bin/clang; do
        command -v "$c" >/dev/null 2>&1 || continue
        if try_cc "$c"; then
            CC=$c
            break
        fi
    done
    : "${CC:=clang}"
else
    try_cc "$CC" || true
fi
: "${SANITIZERS:=fuzzer,address,undefined}"
FLAGS="-g -O1 -fsanitize=$SANITIZERS -fno-sanitize-recover=all -Isrc -Itest -I$B"
{
    (cd tools && xxd -i -n k_pdb_template pdb_app_template.bin)
    (cd tools && xxd -i -n k_pdb_patch_template pdb_patch_template.bin)
    (cd tools && xxd -i -n k_pdb_addcont_template pdb_addcont_template.bin)
} | sed 's/^unsigned/static const unsigned/' > "$B/pdb_template.h"
{
    (cd web && xxd -i -n index_html index.html)
    (cd web && xxd -i -n legacy_html legacy.html)
} > "$B/page_data.h"

fail=0
for t in $TARGETS; do
    case "$t" in
        sfo) src="src/pkg.c src/mem.c src/crypto.c" ;;
        pkg) src="src/pkg.c src/mem.c src/lang.c src/check.c src/crypto.c" ;;
        origin) src="src/origin.c" ;;
        http) src="src/web.c src/lang.c src/build.c src/mem.c src/crypto.c test/stubs.c" ;;
        # BearSSL calls through its vtables with generic types, which -fsanitize=function flags
        rar5) src="-DRAR5DEC_FUZZ -fno-sanitize=function -Ithird_party/bearssl/inc -Ithird_party/bearssl/src src/rar5dec.c src/mem.c $(echo third_party/bearssl/src/{hash/*.c,mac/hmac.c,symcipher/aes_*.c,codec/*.c,settings.c})" ;;
        *) echo "unknown fuzz target: $t" >&2; exit 2 ;;
    esac
    mkdir -p "$B/$t/corpus"
    cp -n test/fuzz/seeds/$t/* "$B/$t/corpus/" 2>/dev/null || true
    $CC $FLAGS -o "$B/$t/fuzz_$t" "test/fuzz/fuzz_$t.c" ${src}
    echo "== fuzz $t for ${SECONDS_EACH}s"
    if ! (cd "$B/$t" && ASAN_OPTIONS=detect_leaks=0 ./fuzz_$t -max_total_time="$SECONDS_EACH" -timeout=10 \
            -rss_limit_mb=2048 -print_final_stats=1 corpus); then
        echo "::error::fuzz target $t found a problem (see build/fuzz/$t)"
        fail=1
    fi
done
exit $fail
