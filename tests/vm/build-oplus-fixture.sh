#!/bin/sh
# The input is the recorded vendor ELF, not an arbitrary module or source tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
[ "$#" -eq 1 ] || { echo "Usage: $0 /path/to/oplus_sys_stability_helper.ko" >&2; exit 2; }
out=${DS_OPLUS_FIXTURE_OUT:-out/compat/fixture}
case "$out" in out/*) ;; *) exit 2;; esac
case "$out" in *..*) exit 2;; esac
python3 "$root/tests/vm/prepare-oplus-fixture.py" "$1" "$root/$out"
cp "$root/tests/vm/oplus-pidref-fixture.c" "$root/$out/fixture.c"
cat >"$root/$out/Makefile" <<'MAKE'
obj-m := oplus_sys_stability_helper.o
oplus_sys_stability_helper-y := fixture.o signal-fixture.o
MAKE
exec docker run --rm --platform linux/amd64 -v "$root:/module" \
    -e FIXTURE="/module/$out" --entrypoint sh \
    ghcr.io/ylarod/ddk-min@sha256:4dd088dc94b391c926cf09215e0a5958f3dfe7d8eeed3616f9041224f85b1d1a \
    -eu -c 'make -C /opt/ddk/kdir/android16-6.12 M="$FIXTURE" LLVM=1 KBUILD_MODPOST_WARN=1 -j4 modules'
