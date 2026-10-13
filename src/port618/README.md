# Android 17 / Linux 6.18 port

Independent implementation against the pinned GKI baseline in [ORIGIN](ORIGIN).
Build with `python3 scripts/build-gki.py 6.18 -j2` from the repository root.

The port uses the 6.18 namespace common metadata, namespace trees and PID limits.
It preserves native pidfd commands outside the added PID/IPC namespace handlers.
Entry patches retain KCFI, BTI, PAC and shadow call stack conventions.

QEMU validation is bound to the exact module and kernel hashes. The KernelSU
loader selects android17-6.18 and checks host BTF before activation. This does
not establish compatibility with every vendor kernel sharing the KMI.
