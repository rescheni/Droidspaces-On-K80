#!/bin/sh
# Run only in QEMU. The rootfs drive is always a disposable snapshot.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
log_dir=${DS_NETFILTER_LOG_DIR:-out/netfilter/vm}
case "$log_dir" in out/*) ;; *) exit 2;; esac
case "$log_dir" in *..*) exit 2;; esac
mkdir -p "$root/$log_dir"
docker run --rm --platform linux/arm64 -v "$root:/work" \
    -e KERNEL="${DS_NETFILTER_KERNEL:-/work/out/gki/android16-6.12/Image}" \
    -e NAMESPACES="${DS_NETFILTER_NAMESPACES:-/work/out/userns-live/droidspaces_ns.ko}" \
    -e MODULE="${DS_NETFILTER_MODULE:-/work/out/netfilter/droidspaces_netfilter.ko}" \
    -e ROOTFS="${DS_NETFILTER_ROOTFS:-/work/out/docker-compat/rootfs.img}" \
    -e LOG="/work/$log_dir/console.log" \
    droidspaces-lkm-vm:local sh -eu -c '
    test -r "$KERNEL" && test -r "$NAMESPACES" && test -r "$MODULE" && test -r "$ROOTFS"
    mkdir -p /tmp/root/bin /tmp/root/dev /tmp/root/proc /tmp/root/sys /tmp/root/tmp
    cp /usr/bin/busybox /tmp/root/bin/
    for a in $(busybox --list); do test "$a" = busybox || ln -s busybox /tmp/root/bin/$a; done
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/load-module.c -o /tmp/root/load-module
    cp "$NAMESPACES" /tmp/root/namespaces.ko
    cp "$MODULE" /tmp/root/netfilter.ko
    cp /work/tests/vm/netfilter-init /tmp/root/init
    cp /work/tests/vm/netfilter-probe /tmp/root/probe
    cp /work/tests/vm/netfilter-rules /work/tests/vm/netfilter-packets /tmp/root/
    chmod +x /tmp/root/init
    mknod /tmp/root/dev/console c 5 1
    cd /tmp/root
    find . -print0 | cpio --null -o -H newc >/tmp/initramfs
    timeout 600 qemu-system-aarch64 -machine virt -cpu max -m 2048 -smp 2 \
        -nographic -monitor none -nic none -no-reboot -kernel "$KERNEL" \
        -initrd /tmp/initramfs -drive "file=$ROOTFS,if=none,id=root,format=raw,snapshot=on" \
        -device nvme,drive=root,serial=ds-test \
        -append "console=ttyAMA0 rdinit=/init nokaslr selinux=0 panic=-1" >"$LOG" 2>&1
    '
log="$root/$log_dir/console.log"
for marker in DS_NFT_RULES_PASS DS_NETFILTER_UNLOAD_PASS \
    DS_BACKEND_PACKETS_PASS=legacy DS_BACKEND_PACKETS_PASS=nft \
    DS_BACKEND_PACKETS_PASS=native-nft DS_NETFILTER_VM_RESULT=0; do
    grep -q "^$marker" "$log" || { tail -80 "$log"; exit 1; }
done
if grep -Eq 'WARNING: CPU:|BUG:|Oops:|Kernel panic|refcount_t:|ISOLATION_FAILED' "$log"; then
    tail -80 "$log"; exit 1
fi
printf 'Netfilter VM regression passed: %s\n' "$log"
