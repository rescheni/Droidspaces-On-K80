#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
log_dir=${DS_USERNS_LOG_DIR:-out/userns/vm}
case "$log_dir" in out/*) ;; *) exit 2;; esac
case "$log_dir" in *..*) exit 2;; esac
mkdir -p "$root/$log_dir"
status=0
docker run --rm --platform linux/arm64 -v "$root:/work" \
    -e KERNEL="${DS_USERNS_KERNEL:-/work/out/gki/android16-6.12/Image}" \
    -e MODULE="${DS_USERNS_MODULE:-/work/out/userns/droidspaces_userns_test.ko}" \
    -e NS_MODULE="${DS_NS_MODULE:-}" -e NS_PARAMS="${DS_NS_PARAMS:-gki=1 activate=1}" \
    -e DEV_MODULE="${DS_DEV_MODULE:-}" \
    -e LOG="/work/$log_dir/console.log" \
    droidspaces-lkm-vm:local sh -eu -c '
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/load-module.c -o /tmp/load-module
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-core.c -o /tmp/userns-core-test
    mkdir -p /tmp/root/bin /tmp/root/dev /tmp/root/proc /tmp/root/sys
    cp /usr/bin/busybox /tmp/root/bin/busybox
    for app in $(busybox --list); do test "$app" = busybox || ln -s busybox "/tmp/root/bin/$app"; done
    cp /tmp/load-module /tmp/root/load-module
    cp /tmp/userns-core-test /tmp/root/userns-core-test
    cp "$MODULE" /tmp/root/userns.ko
    if [ -n "$NS_MODULE" ]; then
        cp "$NS_MODULE" /tmp/root/namespaces.ko
        printf "%s\n" "$NS_PARAMS" > /tmp/root/ns-params
    fi
    if [ -n "$DEV_MODULE" ]; then cp "$DEV_MODULE" /tmp/root/devtmpfs.ko; fi
    cp /work/tests/vm/userns-core-init /tmp/root/init
    chmod +x /tmp/root/init
    mknod /tmp/root/dev/console c 5 1
    cd /tmp/root
    find . -print0 | cpio --null -o --format=newc > /tmp/initramfs
    timeout 180 qemu-system-aarch64 -machine virt -cpu max -m 2048 -smp 2 \
        -nographic -monitor none -nic none -no-reboot -kernel "$KERNEL" -initrd /tmp/initramfs \
        -append "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init nokaslr selinux=0 panic=-1" \
        > "$LOG" 2>&1
    ' || status=$?
tail -n 65 "$root/$log_dir/console.log"
test "$status" = 0
grep -q '^DS_USERNS_CORE_VM_RESULT=0' "$root/$log_dir/console.log"
test "$(grep -c 'CORE_EXIT: remaining=0 creds=0' "$root/$log_dir/console.log")" = 2
! grep -Eq 'BUG:|WARNING:|Oops|Kernel panic' "$root/$log_dir/console.log"
