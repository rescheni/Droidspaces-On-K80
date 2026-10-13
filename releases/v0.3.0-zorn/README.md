# v0.3.0-zorn 构建产物

一次已验证构建的现成产物，直接对应
`build/zorn-6.1.json` 中固定的源码、模块与设备证据哈希。

| 文件 | 用途 |
| --- | --- |
| `Droidspaces-LKM-KernelSU-v0.3.0-zorn.zip` | 在 KernelSU 管理器中安装，重启后自动加载 |
| `droidspaces_ns-android14-6.1-zorn.ko` | 独立载荷（自行 `insmod` 时使用） |
| `SHA256SUMS` | 上面两个文件的校验值 |

适用内核，必须完全一致：

```text
6.1.138-android14-11-g965475777129-mi
```

内核 BTF 指纹：

```text
sha256(/sys/kernel/btf/vmlinux) = 80a145adb8665abc5fc495250843144f5b006caf43c62a080ea34ca8fa0079bb
```

安装与重启后的操作、故障排查见 [zorn 6.1 设备包](../docs/zorn-6.1.md#安装与重启)，
实现原理见 [实现机制](../docs/mechanism.md)。

重新构建（需要解包后的 DDK）：

```sh
DDK=$HOME/opt/ddk/rootfs/opt/ddk
scripts/build-local-ddk.sh $DDK/kdir/android14-6.1 $DDK/src/android14-6.1 $DDK \
    TARGET=namespace PORT_DIR=src/port61full \
    GKI_ANDROID=android14 GKI_VERSION=6.1 OUT=$PWD/out/port61full -j4
python3 scripts/package-zorn.py
```

> GitHub Releases 更适合放二进制；这里保留一份是为了让仓库自身可用。
