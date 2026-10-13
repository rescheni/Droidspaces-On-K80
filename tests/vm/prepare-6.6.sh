#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$root/out/vm-6.6"
docker build --platform linux/arm64 -t droidspaces-lkm-vm:local "$root/tests/vm"
docker run --rm --platform linux/amd64 -v "$root:/module" \
    ghcr.io/ylarod/ddk-min@sha256:51159bc935c75a282fc65bc87e02f16bd098d1cdf55dc558c4fdd0fcf7a132e2 \
    sh -eu -c '
    llvm-objcopy -O binary -R .note -R .note.gnu.build-id -R .comment -S \
        "$KDIR/vmlinux" /module/out/vm-6.6/Image
    cp "$KDIR/.config" /module/out/vm-6.6/kernel.config
    cp "$KDIR/include/config/kernel.release" /module/out/vm-6.6/kernel.release
    '
