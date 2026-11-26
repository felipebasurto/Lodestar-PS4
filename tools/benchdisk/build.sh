#!/bin/bash
# Builds build/benchdisk.elf and build/benchdisk.bin, the standalone read-only disk diagnostic.
# The artifact is deliberately not part of the daemon build or release payload.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

export PS4_PAYLOAD_SDK="${PS4_PAYLOAD_SDK:-$ROOT/build/sdk-local}"
if [ -z "${LLVM_CONFIG:-}" ] && [ -x /opt/homebrew/opt/llvm@17/bin/llvm-config ]; then
    export LLVM_CONFIG=/opt/homebrew/opt/llvm@17/bin/llvm-config
fi
B="$PS4_PAYLOAD_SDK/bin"

GIT_BUILD=$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    GIT_BUILD="$GIT_BUILD+dirty"
fi

mkdir -p build
CFLAGS=(
    -O2 -Wall -Wextra -Werror -std=c11
    -I"$ROOT/src"
    "-DBENCH_BUILD=\"$GIT_BUILD\""
    "-DBENCH_SDK=\"573b4a0d\""
)
if [ -n "${BENCH_AC_USB:-}" ]; then
    CFLAGS+=("-DBENCH_AC_USB=\"$BENCH_AC_USB\"")
fi
if [ -n "${BENCH_AC_MNT:-}" ]; then
    CFLAGS+=("-DBENCH_AC_MNT=\"$BENCH_AC_MNT\"")
fi

"$B/orbis-clang" "${CFLAGS[@]}" \
    -o build/benchdisk.elf \
    tools/benchdisk/benchdisk.c src/mem.c \
    -lkernel_sys

"$B/orbis-clang" -c -Os -fPIC -ffreestanding -fno-builtin -fno-jump-tables -fno-stack-protector \
    -fno-asynchronous-unwind-tables -mstackrealign -Wall -Wextra -Werror \
    -o build/binboot.o tools/binboot/binboot.c
"$B/orbis-ld" --static -T tools/binboot/binboot.ld -o build/binboot.elf build/binboot.o
"$B/llvm-objcopy" -O binary --only-section=.text build/binboot.elf build/binboot.bin
cat build/binboot.bin build/benchdisk.elf > build/benchdisk.bin

ls -la build/benchdisk.elf build/benchdisk.bin
shasum -a 256 build/benchdisk.elf build/benchdisk.bin
