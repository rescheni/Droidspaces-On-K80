# Android 13 / Linux 5.15 port

Independent source set for the pinned GKI baseline in [ORIGIN](ORIGIN).
Build with `python3 scripts/build-gki.py 5.15 -j2` from the repository root.
The KernelSU loader selects this payload for android13-5.15 and checks BTF
compatibility before loading with `gki=1 activate=1`.

QEMU validation is recorded against the exact output module and kernel hashes;
this does not establish compatibility with every vendor kernel sharing the KMI.
