#!/bin/bash
# Build a module against an extracted DDK without docker.
#
# The DDK's generated build dir includes its source Makefile through the
# absolute path /opt/ddk/src/..., so the extracted rootfs is bind-mounted at
# /opt/ddk inside a private user+mount namespace. No root, no docker, no host
# writes.
#
# Usage: scripts/build-local-ddk.sh KDIR SRCTREE DDKROOT [make args...]
#   KDIR     generated build dir, e.g. $DDKROOT/kdir/android14-6.1
#   SRCTREE  matching kernel source tree, e.g. $DDKROOT/src/android14-6.1
#   DDKROOT  extracted DDK root (the directory that holds clang/, kdir/, src/)
#
# Two host programs in the image are linked against a newer glibc than this
# host provides, so they are prepared before the kernel build runs:
#   modpost, resolve_btfids - scripts/fix-ddk-hosttools.sh
#   pahole                  - run through the image's own loader (out/tools)
set -eu

K=$(cd -- "${1:?usage: build-local-ddk.sh KDIR SRCTREE DDKROOT [make args...]}" && pwd)
SRC=$(cd -- "${2:?}" && pwd)
DDK=$(cd -- "${3:?}" && pwd)
shift 3

ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
CLANG=$DDK/clang/clang-r487747c/bin
TOOLS=$ROOT/out/tools
HOSTLIB=${DDK_HOSTLIB:-$HOME/opt/ddk/hostlib}

[ -x "$CLANG/clang" ] || { echo "missing toolchain: $CLANG" >&2; exit 1; }
[ -x "$HOSTLIB/usr/bin/pahole" ] || { echo "missing image pahole: $HOSTLIB" >&2; exit 1; }

"$ROOT/scripts/fix-ddk-hosttools.sh" "$K" "$SRC" "$HOSTLIB"

mkdir -p "$TOOLS"
printf '#!/bin/sh\nexec %s/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 --library-path %s/usr/lib/x86_64-linux-gnu %s/usr/bin/pahole "$@"\n' \
	"$HOSTLIB" "$HOSTLIB" "$HOSTLIB" > "$TOOLS/pahole"
chmod +x "$TOOLS/pahole"

exec env TOOLS="$TOOLS" CLANG="$CLANG" DDK="$DDK" ROOT="$ROOT" \
	unshare -rm --propagation private /bin/bash -s -- "$@" <<'EOS'
set -eu
mount -t tmpfs none /opt
mkdir -p /opt/ddk
mount --bind "$DDK" /opt/ddk
export PATH="$CLANG:$TOOLS:$PATH"
export ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu-
export KDIR=/opt/ddk/kdir/android14-6.1
cd "$ROOT"
exec make "$@"
EOS
