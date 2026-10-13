# Android 12 / Linux 5.10 port

Independent implementation against the pinned GKI baseline in [ORIGIN](ORIGIN).
Build with `python3 scripts/build-gki.py 5.10 -j2` from the repository root.

The KernelSU loader selects android12-5.10, checks host BTF when available,
and requires an exact validated kernel build fingerprint when BTF is absent.
The pinned baseline has no runtime BTF; user-space BPF tracing programs are
therefore not available, while perf, cgroup and FUSE lifetime tests remain enabled.

QEMU validation is tied to the exact module and kernel hashes. It does not
establish compatibility with every vendor kernel sharing the KMI.
