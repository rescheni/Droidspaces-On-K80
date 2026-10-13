#!/bin/bash
# Cross-compile the freestanding AArch64 namespace probe with the DDK clang.
#
#   tests/device/build-nsprobe.sh OUTPUT [DDKROOT]
#
# The probe calls the replaced system calls directly, so it needs no libc and
# no device-side toolchain.
set -eu

OUT=${1:?usage: build-nsprobe.sh OUTPUT [DDKROOT]}
DDK=${2:-$HOME/opt/ddk/rootfs/opt/ddk}
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
CLANG=$DDK/clang/clang-r487747c/bin

[ -x "$CLANG/clang" ] || { echo "missing toolchain: $CLANG" >&2; exit 1; }

LD_LIBRARY_PATH=$DDK/clang/clang-r487747c/lib \
	"$CLANG/clang" --target=aarch64-linux-gnu -nostdlib -static -O2 \
	-fno-stack-protector -fno-builtin -Wall -Werror -Wl,--build-id=none \
	-o "$OUT" "$HERE/nsprobe.c"
echo "probe: $OUT"
