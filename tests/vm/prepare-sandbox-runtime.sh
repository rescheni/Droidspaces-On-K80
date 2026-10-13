#!/bin/sh
# Fetch the Debian arm64 Bubblewrap executable and its runtime for offline QEMU.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$root/out/userns-live/sandbox-runtime"
docker run --rm --platform linux/arm64 \
    -v "$root/out/userns-live/sandbox-runtime:/out" \
    droidspaces-lkm-vm:local sh -eu -c '
    apt-get update
    apt-get install -y --no-install-recommends bubblewrap
    mkdir -p /out/root/usr/bin
    cp /usr/bin/bwrap /out/root/usr/bin/bwrap
    ldd /usr/bin/bwrap > /out/ldd.txt
    awk '\''{for(i=1;i<=NF;i++) if($i ~ /^\//) print $i}'\'' /out/ldd.txt |
        while IFS= read -r library; do cp -L --parents "$library" /out/root; done
    dpkg-query -W -f='\''${binary:Package} ${Version}\n'\'' > /out/packages.txt
    cd /out/root
    find . -type f -exec sha256sum {} + > /out/SHA256SUMS
    /usr/bin/bwrap --version
    '
