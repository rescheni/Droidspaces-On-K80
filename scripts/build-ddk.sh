#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# android16-6.12-20260313, pinned to the image used for the device test.
image=ghcr.io/ylarod/ddk-min@sha256:4dd088dc94b391c926cf09215e0a5958f3dfe7d8eeed3616f9041224f85b1d1a
exec docker run --rm --platform linux/amd64 \
    -v "$root:/module" -w /module "$image" \
    make OUT=/module/out/android16-6.12 "$@"
