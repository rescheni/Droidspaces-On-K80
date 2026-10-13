#!/bin/sh
# Build a static runtime from a read-only checkout for disposable VM tests.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source_root=$(CDPATH= cd -- "${1:-$root/../droidspaces-oss}" && pwd)
out="$root/out/userns-live/droidspaces-runtime"
mkdir -p "$out"
git -C "$source_root" rev-parse HEAD > "$out/source-commit.txt"
git -C "$source_root" status --short > "$out/source-status.txt"
docker run --rm --platform linux/arm64 \
    -v "$source_root:/input:ro" -v "$out:/out" \
    droidspaces-lkm-vm:local sh -eu -c '
    apt-get update
    apt-get install -y --no-install-recommends make musl-tools
    ln -s /usr/include/linux /usr/include/aarch64-linux-musl/linux
    ln -s /usr/include/asm-generic /usr/include/aarch64-linux-musl/asm-generic
    ln -s /usr/include/aarch64-linux-gnu/asm /usr/include/aarch64-linux-musl/asm
    mkdir /tmp/source
    cp -a /input/src /input/Makefile /tmp/source/
    cd /tmp/source
    find src -type f -exec sha256sum {} + | LC_ALL=C sort > /out/source-SHA256SUMS
    sha256sum Makefile >> /out/source-SHA256SUMS
    make native NATIVE_CC=musl-gcc NPROC=2
    cp output/droidspaces /out/
    dpkg-query -W -f='\''${binary:Package} ${Version}\n'\'' > /out/packages.txt
    cd /out
    sha256sum droidspaces > SHA256SUMS
    ./droidspaces help > help.txt
    ' > "$out/build.log" 2>&1
cat "$out/SHA256SUMS"
