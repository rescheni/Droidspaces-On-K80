#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export DS_VM_KERNEL=/work/out/vm-6.6/Image
export DS_VM_MODULE=/work/out/android15-6.6-ns/droidspaces_ns.ko
export DS_VM_LOG_DIR=out/vm-6.6
export DS_TEST_PIDFD_NS_IOCTL=0
export DS_TEST_SHM_HUGETLB_REJECT=1
exec "$root/tests/vm/run.sh"
