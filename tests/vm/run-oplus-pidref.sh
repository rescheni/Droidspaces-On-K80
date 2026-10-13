#!/bin/sh
# Fixture generation requires the recorded vendor ELF; it is never loaded as-is.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
log_dir=${DS_OPLUS_LOG_DIR:-out/compat/vm}
case "$log_dir" in out/*) ;; *) exit 2;; esac
case "$log_dir" in *..*) exit 2;; esac
mkdir -p "$root/$log_dir"
docker run --rm --platform linux/arm64 -v "$root:/work" \
    -e KERNEL="${DS_OPLUS_KERNEL:-/work/out/gki/android16-6.12/Image}" \
    -e NAMESPACES="${DS_OPLUS_NAMESPACES:-/work/out/userns-live/droidspaces_ns.ko}" \
    -e MODULE="${DS_OPLUS_MODULE:-/work/out/compat/droidspaces_compat.ko}" \
    -e FIXTURE="${DS_OPLUS_FIXTURE:-/work/out/compat/fixture/oplus_sys_stability_helper.ko}" \
    -e NEGATIVE="${DS_OPLUS_NEGATIVE:-0}" -e LOG="/work/$log_dir/console.log" \
    droidspaces-lkm-vm:local sh -eu -c '
    mkdir -p /tmp/root/bin /tmp/root/dev /tmp/root/proc /tmp/root/sys /tmp/root/tmp
    cp /usr/bin/busybox /tmp/root/bin/
    for a in $(busybox --list); do test "$a" = busybox || ln -s busybox /tmp/root/bin/$a; done
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/load-module.c -o /tmp/root/load-module
    gcc -static -pthread -O2 -Wall -Wextra -Werror /work/tests/vm/oplus-pidref.c -o /tmp/root/oplus-pidref
    cp "$NAMESPACES" /tmp/root/namespace.ko
    cp "$MODULE" /tmp/root/compat.ko
    cp "$FIXTURE" /tmp/root/fixture.ko
    cp /work/tests/vm/oplus-pidref-init /tmp/root/init
    chmod +x /tmp/root/init
    mknod /tmp/root/dev/console c 5 1
    mknod /tmp/root/dev/null c 1 3
    chmod 666 /tmp/root/dev/null
    cd /tmp/root
    find . -print0 | cpio --null -o -H newc >/tmp/initramfs
    timeout 180 qemu-system-aarch64 -machine virt -cpu max -m 2048 -smp 2 \
        -nographic -monitor none -nic none -no-reboot -kernel "$KERNEL" \
        -initrd /tmp/initramfs \
        -append "console=ttyAMA0 rdinit=/init nokaslr selinux=0 panic=-1 ds_negative=$NEGATIVE" >"$LOG" 2>&1
    '
log="$root/$log_dir/console.log"
if [ "${DS_OPLUS_NEGATIVE:-0}" = 1 ]; then marker=DS_OPLUS_BASELINE_LEAK_PASS; else marker=DS_OPLUS_PIDREF_PASS; fi
grep -q "^$marker" "$log" && grep -q '^DS_OPLUS_VM_RESULT=0' "$log" || { tail -80 "$log"; exit 1; }
if grep -Eq 'WARNING: CPU:|BUG:|Oops:|Kernel panic|refcount_t:' "$log"; then tail -80 "$log"; exit 1; fi
printf 'Oplus PID ownership regression passed: %s\n' "$log"
