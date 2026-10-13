#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/out/userns-device-test"
exec docker run --rm --platform linux/arm64 -v "$root:/work" \
    droidspaces-lkm-vm:local gcc -static -O2 -Wall -Wextra -Werror \
    /work/src/health/userns.c -o /work/out/userns-device-test/userns-check
