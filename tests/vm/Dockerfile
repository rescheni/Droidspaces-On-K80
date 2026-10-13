FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-arm busybox-static cpio gcc libc6-dev python3 \
    && rm -rf /var/lib/apt/lists/*
