#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$root/out/vm"
docker build --platform linux/arm64 -t droidspaces-lkm-vm:local "$root/tests/vm"
docker run --rm --platform linux/amd64 -v "$root:/module" \
    ghcr.io/ylarod/ddk-min@sha256:4dd088dc94b391c926cf09215e0a5958f3dfe7d8eeed3616f9041224f85b1d1a \
    sh -eu -c '
    llvm-objcopy -O binary -R .note -R .note.gnu.build-id -R .comment -S \
        "$KDIR/vmlinux" /module/out/vm/Image
    cp "$KDIR/.config" /module/out/vm/kernel.config
    '
