#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
log_dir=${DS_USERNS_LOG_DIR:-out/userns-live/vm}
case "$log_dir" in out/*) ;; *) exit 2;; esac
case "$log_dir" in *..*) exit 2;; esac
mkdir -p "$root/$log_dir"
export_root=${DS_USERNS_EXPORT_ROOT:-}
if [ -n "$export_root" ]; then
    case "$export_root" in out/*) ;; *) exit 2;; esac
    case "$export_root" in *..*) exit 2;; esac
    mkdir -p "$root/$(dirname "$export_root")"
    export_root="/work/$export_root"
fi
status=0
docker run --rm --platform linux/arm64 -v "$root:/work" \
    -e KERNEL="${DS_USERNS_KERNEL:-/work/out/gki/android16-6.12/Image}" \
    -e MODULE="${DS_USERNS_MODULE:-/work/out/userns-live/droidspaces_ns.ko}" \
    -e HMA="${DS_USERNS_HMA:-}" \
    -e EXPORT_ROOT="$export_root" \
    -e DROIDSPACES="${DS_USERNS_DROIDSPACES:-0}" \
    -e SANDBOX="${DS_USERNS_SANDBOX:-0}" \
    -e LOG="/work/$log_dir/console.log" \
    droidspaces-lkm-vm:local sh -eu -c '
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/load-module.c -o /tmp/load-module
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-live.c -o /tmp/userns-live-test
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-overlay.c -o /tmp/userns-overlay-test
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-fuse.c -o /tmp/userns-fuse-test
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-fuse-io.c -o /tmp/userns-fuse-io-test
    gcc -static -pthread -O2 -Wall -Wextra -Werror /work/tests/vm/userns-policy.c -o /tmp/userns-policy-test
    mkdir -p /tmp/root/bin /tmp/root/dev /tmp/root/proc /tmp/root/sys /tmp/root/tmp
    chmod 1777 /tmp/root/tmp
    cp /usr/bin/busybox /tmp/root/bin/busybox
    for app in $(busybox --list); do test "$app" = busybox || ln -s busybox "/tmp/root/bin/$app"; done
    cp /tmp/load-module /tmp/root/load-module
    cp /tmp/userns-live-test /tmp/root/userns-live-test
    cp /tmp/userns-overlay-test /tmp/root/userns-overlay-test
    cp /tmp/userns-fuse-test /tmp/root/userns-fuse-test
    cp /tmp/userns-fuse-io-test /tmp/root/userns-fuse-io-test
    mknod /tmp/root/dev/fuse c 10 229
    chmod 666 /tmp/root/dev/fuse
    cp /tmp/userns-policy-test /tmp/root/userns-policy-test
    cp /tmp/userns-policy-test /tmp/root/userns-policy-suid-host
    chmod 4755 /tmp/root/userns-policy-suid-host
    cp /tmp/userns-policy-test /tmp/root/userns-policy-sgid-host
    chmod 2755 /tmp/root/userns-policy-sgid-host
    cp /tmp/userns-policy-test /tmp/root/userns-policy-suid-mapped
    chown 1000:1000 /tmp/root/userns-policy-suid-mapped
    chmod 4755 /tmp/root/userns-policy-suid-mapped
    mkdir -p /tmp/ext4-root /tmp/root/test-ext4
    cp /tmp/userns-policy-test /tmp/ext4-root/userns-policy-fcaps
    chown 1000:1000 /tmp/ext4-root/userns-policy-fcaps
    truncate -s 32M /tmp/root/filecaps.ext4
    mkfs.ext4 -q -F -d /tmp/ext4-root /tmp/root/filecaps.ext4
    mknod /tmp/root/dev/loop0 b 7 0
    mknod /tmp/root/dev/loop-control c 10 237
    if [ "$SANDBOX" != 0 ]; then
        cd /work/out/userns-live/sandbox-runtime/root
        sha256sum -c ../SHA256SUMS
        cd /
        cp -a /work/out/userns-live/sandbox-runtime/root/. /tmp/root/
        gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/userns-sandbox.c -o /tmp/root/sandbox-test
        if [ "$SANDBOX" = all ]; then touch /tmp/root/sandbox-all-enabled; fi
        touch /tmp/root/sandbox-enabled /tmp/root/host-should-be-hidden
        mkdir -p /tmp/root/tmp
        chmod 1777 /tmp/root/tmp
        mknod /tmp/root/dev/null c 1 3
        mknod /tmp/root/dev/zero c 1 5
        mknod /tmp/root/dev/full c 1 7
        mknod /tmp/root/dev/random c 1 8
        mknod /tmp/root/dev/urandom c 1 9
        mknod /tmp/root/dev/tty c 5 0
        chmod 666 /tmp/root/dev/null /tmp/root/dev/zero /tmp/root/dev/full /tmp/root/dev/random /tmp/root/dev/urandom /tmp/root/dev/tty
    fi
    if [ "$DROIDSPACES" = 1 ]; then
        test "$SANDBOX" = all
        cd /work/out/userns-live/droidspaces-runtime
        sha256sum -c SHA256SUMS
        cd /
        cp /work/out/userns-live/droidspaces-runtime/droidspaces /tmp/root/droidspaces
        cp /work/tests/vm/droidspaces-sandbox /tmp/root/droidspaces-sandbox
        chmod +x /tmp/root/droidspaces-sandbox
        mkdir -p /tmp/root/container/bin /tmp/root/container/sbin /tmp/root/container/etc/profile.d \
            /tmp/root/container/dev /tmp/root/container/proc /tmp/root/container/sys \
            /tmp/root/container/tmp /tmp/root/container/run /tmp/root/container/root
        cp -a /tmp/root/bin/. /tmp/root/container/bin/
        cp -a /work/out/userns-live/sandbox-runtime/root/. /tmp/root/container/
        cp /tmp/root/sandbox-test /tmp/root/container/sandbox-test
        cp /tmp/root/userns-fuse-io-test /tmp/root/container/userns-fuse-io-test
        cp /work/tests/vm/droidspaces-sandbox-init /tmp/root/container/sbin/init
        chmod +x /tmp/root/container/sbin/init
        chmod 1777 /tmp/root/container/tmp
        touch /tmp/root/container/host-should-be-hidden
        echo "NAME=DroidspacesSandboxTest" > /tmp/root/container/etc/os-release
        echo "root:x:0:0:root:/root:/bin/sh" > /tmp/root/container/etc/passwd
        echo "sandbox:x:1000:1000:sandbox:/tmp:/bin/sh" >> /tmp/root/container/etc/passwd
        echo "root:x:0:" > /tmp/root/container/etc/group
        echo "sandbox:x:1000:" >> /tmp/root/container/etc/group
        mknod /tmp/root/dev/ptmx c 5 2
        chmod 666 /tmp/root/dev/ptmx
    fi
    if [ -n "$HMA" ]; then cp "$HMA" /tmp/root/hma.ko; fi
    cp "$MODULE" /tmp/root/namespaces.ko
    cp /work/tests/vm/userns-live-init /tmp/root/init
    chmod +x /tmp/root/init
    mknod /tmp/root/dev/console c 5 1
    if [ -n "$EXPORT_ROOT" ]; then
        # Hardware tests invoke an explicit runner, never the VM poweroff init.
        tar -C /tmp/root --exclude=./init --exclude=./namespaces.ko --exclude=./load-module --exclude=./hma.ko -czf "$EXPORT_ROOT" .
        cp /usr/bin/busybox "$EXPORT_ROOT.busybox"
    fi
    cd /tmp/root
    find . -print0 | cpio --null -o --format=newc > /tmp/initramfs
    timeout 180 qemu-system-aarch64 -machine virt -cpu max -m 2048 -smp 2 \
        -nographic -monitor none -nic none -no-reboot -kernel "$KERNEL" -initrd /tmp/initramfs \
        -append "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init nokaslr selinux=0 panic=-1" \
        > "$LOG" 2>&1
    ' || status=$?
tail -n 65 "$root/$log_dir/console.log"
test "$status" = 0
grep -q '^DS_USERNS_LIVE_VM_RESULT=0' "$root/$log_dir/console.log"
for marker in DS_FUSE_PASS DS_FUSE_SANDBOX_MOUNT_PASS DS_FUSE_ACL_PASS DS_FUSE_RETAINED_PASS DS_OVERLAY_PASS; do
    grep -q "^$marker" "$root/$log_dir/console.log"
done
grep -q '^DS_FUSE_IO_PASS: unprivileged' "$root/$log_dir/console.log"
grep -q '^DS_FUSE_IO_PASS: mapped-range' "$root/$log_dir/console.log"
grep -q 'DS_USERNS_POLICY_PASS:' "$root/$log_dir/console.log"
grep -q 'DS_USERNS_LIVE_PASS:' "$root/$log_dir/console.log"
! grep -Eq 'BUG:|WARNING:|Oops|Kernel panic' "$root/$log_dir/console.log"
if [ "${DS_USERNS_SANDBOX:-0}" != 0 ]; then
    grep -q "^DS_FUSE_BUBBLEWRAP_PASS" "$root/$log_dir/console.log"
    grep -q 'DS_USERNS_SANDBOX_PASS:' "$root/$log_dir/console.log"
fi
if [ "${DS_USERNS_SANDBOX:-0}" = all ]; then
    grep -q 'DS_USERNS_SANDBOX_DISABLED_PASS:' "$root/$log_dir/console.log"
    grep -q 'DS_USERNS_SANDBOX_ALL_PASS:' "$root/$log_dir/console.log"
fi
if [ "${DS_USERNS_DROIDSPACES:-0}" = 1 ]; then
    grep -q 'DS_USERNS_DROIDSPACES_PASS:' "$root/$log_dir/console.log"
fi

if [ -n "${DS_USERNS_HMA:-}" ]; then
    grep -q 'preserving HMA setuid entry; replacing native resume body' "$root/$log_dir/console.log"
fi
