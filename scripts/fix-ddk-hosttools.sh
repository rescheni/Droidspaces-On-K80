#!/bin/bash
# Make the extracted DDK build tree usable on a host whose glibc is older
# than the image's (Debian bookworm, glibc 2.38+).
#
# Two host programs ship inside the image:
#   scripts/mod/modpost                      -> recompiled from pinned source
#   tools/bpf/resolve_btfids/resolve_btfids  -> runs under the image's loader
#
# Nothing here touches the kernel sources; only the image's own prebuilt host
# tools are replaced or wrapped. Both steps are idempotent.
set -eu

K=$(cd -- "${1:?usage: fix-hosttools.sh KDIR SRCTREE [IMGDIR]}" && pwd)
SRC=$(cd -- "${2:?}" && pwd)
IMG=${3:-$HOME/opt/ddk/hostlib}
CC=${CC:-gcc}

LOADER=$IMG/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
IMGLIB=$IMG/usr/lib/x86_64-linux-gnu
[ -x "$LOADER" ] || { echo "missing image loader: $LOADER" >&2; exit 1; }

cd "$K"

# The image's modpost needs glibc 2.38; rebuild it from the pinned source
# using the same commands kbuild recorded in scripts/mod/.*.cmd.
if ! scripts/mod/modpost </dev/null >/dev/null 2>&1; then
	for unit in modpost file2alias sumversion; do
		$CC -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 \
			-fomit-frame-pointer -std=gnu11 -I ./scripts/mod \
			-c -o "scripts/mod/$unit.o" "$SRC/scripts/mod/$unit.c"
	done
	$CC -o scripts/mod/modpost scripts/mod/modpost.o \
		scripts/mod/file2alias.o scripts/mod/sumversion.o
	echo "modpost rebuilt with $($CC --version | head -1)"
fi

# resolve_btfids is invoked by kbuild as a path inside the build tree, so a
# PATH shim cannot intercept it; keep the image binary and put a loader
# trampoline in its place.
wrap_glibc() {
	bin=$1
	[ -x "$bin.image" ] && return 0
	[ -x "$bin" ] || { echo "missing host tool: $bin" >&2; return 1; }
	mv "$bin" "$bin.image"
	{
		echo '#!/bin/sh'
		echo "exec $LOADER --library-path $IMGLIB \"\$0.image\" \"\$@\""
	} > "$bin"
	chmod +x "$bin"
	echo "wrapped $(basename "$bin") with the image loader"
}

btfids=$K/tools/bpf/resolve_btfids/resolve_btfids
if ! "$btfids" -h >/dev/null 2>&1 && "$btfids" -h 2>&1 | grep -q 'GLIBC_2\.'; then
	wrap_glibc "$btfids"
fi

echo "host tools ready in $K"
