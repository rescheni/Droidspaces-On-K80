#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# android15-6.6-20260313; independent toolchain, source set and output directory.
image=ghcr.io/ylarod/ddk-min@sha256:51159bc935c75a282fc65bc87e02f16bd098d1cdf55dc558c4fdd0fcf7a132e2
exec docker run --rm --platform linux/amd64 \
    -v "$root:/module" -w /module "$image" \
    make TARGET=namespace PORT_DIR=src/port66 OUT=/module/out/android15-6.6-ns "$@"
