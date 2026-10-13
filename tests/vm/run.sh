#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
kernel=${DS_VM_KERNEL:-/work/out/vm/Image}
module=${DS_VM_MODULE:-/work/out/android16-6.12-ns/droidspaces_ns.ko}
log_dir=${DS_VM_LOG_DIR:-out/vm}
case "$log_dir" in out/*) ;; *) echo 'DS_VM_LOG_DIR must be under out/' >&2; exit 1;; esac
case "$log_dir" in *..*) echo 'Invalid log directory' >&2; exit 1;; esac
test -f "$root/${kernel#/work/}"
test -f "$root/${module#/work/}"
mkdir -p "$root/$log_dir"
vm_status=0
docker run --rm --platform linux/arm64 -v "$root:/work" \
    -e DS_VM_KERNEL="$kernel" \
    -e DS_VM_MODULE="$module" \
    -e DS_VM_LOG_DIR="/work/$log_dir" \
    -e DS_VM_NATIVE_IPC="${DS_VM_NATIVE_IPC:-0}" \
    -e DS_VM_GKI_DIR="${DS_VM_GKI_DIR:-}" \
    -e DS_TEST_618_INTERFACES="${DS_TEST_618_INTERFACES:-0}" \
    -e DS_TEST_BPF_ITER="${DS_TEST_BPF_ITER:-1}" \
    -e DS_TEST_MEMFD_POLICY="${DS_TEST_MEMFD_POLICY:-1}" \
    -e DS_VM_MODULE_PARAMS="${DS_VM_MODULE_PARAMS:-}" \
    -e DS_TEST_PIDFD_NS_IOCTL="${DS_TEST_PIDFD_NS_IOCTL:-1}" \
    -e DS_TEST_SHM_HUGETLB_REJECT="${DS_TEST_SHM_HUGETLB_REJECT:-0}" \
    droidspaces-lkm-vm:local sh -eu -c '
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/load-module.c -o /tmp/load-module
    gcc -static -O2 -Wall -Wextra -Werror -DDS_TEST_PIDFD_NS_IOCTL="$DS_TEST_PIDFD_NS_IOCTL" /work/tests/vm/namespaces.c -o /tmp/namespaces
    gcc -static -O2 -Wall -Wextra -Werror -DDS_TEST_SHM_HUGETLB_REJECT="$DS_TEST_SHM_HUGETLB_REJECT" /work/tests/vm/ipc.c -o /tmp/ipc-test
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/mqueue.c -lrt -o /tmp/mqueue-test
    gcc -static -O2 -Wall -Wextra -Werror -DDS_TEST_BPF_ITER="$DS_TEST_BPF_ITER" /work/tests/vm/pid-lifetimes.c -o /tmp/pid-lifetimes
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/pid-policy.c -o /tmp/pid-policy
    gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/preexisting-ipc.c -lrt -o /tmp/preexisting-ipc
    mkdir -p /tmp/root/bin /tmp/root/dev /tmp/root/proc /tmp/root/sys
    cp /usr/bin/busybox /tmp/root/bin/busybox
    for app in $(busybox --list); do
        test "$app" = busybox || ln -s busybox "/tmp/root/bin/$app"
    done
    cp /tmp/load-module /tmp/root/load-module
    cp /tmp/namespaces /tmp/root/namespaces
    cp /tmp/ipc-test /tmp/root/ipc-test
    cp /tmp/mqueue-test /tmp/root/mqueue-test
    cp /tmp/pid-lifetimes /tmp/root/pid-lifetimes
    cp /tmp/pid-policy /tmp/root/pid-policy
    cp /tmp/preexisting-ipc /tmp/root/preexisting-ipc
    if [ "$DS_TEST_618_INTERFACES" = 1 ]; then
        gcc -static -O2 -Wall -Wextra -Werror /work/tests/vm/namespaces-6.18.c -o /tmp/root/namespaces-6.18
    fi
    if [ -n "$DS_VM_GKI_DIR" ]; then
        mkdir -p /tmp/root/gki/bin /tmp/root/gki/kernel
        cp /work/kernelsu/common.sh /tmp/root/gki/common.sh
        cp /work/out/gki/bin/abi-check /tmp/root/gki/bin/abi-check
        cp "$DS_VM_GKI_DIR/$(basename "$DS_VM_GKI_DIR").abi" /tmp/root/gki/kernel/profile.abi
        cp "$DS_VM_GKI_DIR/kernel.notes" /tmp/root/gki/kernel/kernel.notes
    fi
    printf "%s\n" "$DS_VM_MODULE_PARAMS" > /tmp/root/module-params
    if [ "$DS_VM_NATIVE_IPC" = 1 ]; then touch /tmp/root/native-ipc; fi
    if [ "$DS_TEST_MEMFD_POLICY" = 0 ]; then touch /tmp/root/no-memfd-policy; fi
    cp /work/tests/vm/init /tmp/root/init
    chmod +x /tmp/root/init
    cp "$DS_VM_MODULE" /tmp/root/droidspaces_ns.ko
    mknod /tmp/root/dev/console c 5 1
    mknod /tmp/root/dev/fuse c 10 229
    cd /tmp/root
    find . -print0 | cpio --null -o --format=newc > /tmp/initramfs
    timeout 180 qemu-system-aarch64 -machine virt -cpu max -m 2048 -smp 2 \
        -nographic -monitor none -nic none -no-reboot \
        -kernel "$DS_VM_KERNEL" -initrd /tmp/initramfs \
        -append "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init nokaslr selinux=0 panic=-1" \
        > "$DS_VM_LOG_DIR/console.log" 2>&1
    ' || vm_status=$?
cat "$root/$log_dir/console.log"
test "$vm_status" -eq 0
grep -q '^DS_VM_RESULT=0' "$root/$log_dir/console.log"
if grep -Eq 'BUG:|WARNING:|Oops|Kernel panic' "$root/$log_dir/console.log"; then
    echo 'Kernel diagnostics found in VM log' >&2
    exit 1
fi
