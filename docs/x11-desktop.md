# Droidspaces 里的 Linux 桌面（Termux:X11）

设备：Redmi K80 Pro（`zorn`），Android 16，KernelSU。
容器：Droidspaces `Arch`（Arch Linux ARM + systemd），已装 bspwm 桌面。

本机当前状态：**已经装好并在运行**。`Termux:X11` 应用 + `termux-x11` 配套包 +
`xkeyboard-config` 已就位，容器内 bspwm / polybar / picom / dunst 已在 `:5` 上跑，
打开 Termux:X11 应用即可看到桌面。

## 组件与分工

| 组件 | 作用 |
| --- | --- |
| `com.termux.x11`（APK） | 显示端：把 X 画面画进一个 Android 窗口/Surface |
| `termux-x11`（Termux 包） | X server 本体，用 `app_process` 以 Termux 的 uid 启动 |
| `xkeyboard-config`（Termux 包） | XKB 键盘布局数据，缺了 server 会报 `$XKB_CONFIG_ROOT is not set` |
| Droidspaces `enable_termux_x11=1` | 把 `X5` socket bind-mount 进容器，并在容器内设 `DISPLAY=:5` |
| 容器内 bspwm 会话 | 窗口管理器 + 顶栏 + 合成器，由容器 systemd 自启 |

数据流：容器内 X 客户端 → `/tmp/.X11-unix/X5`（bind-mount）→ 宿主
`/data/data/com.termux/files/usr/tmp/.X11-unix/X5` → `termux-x11` server → Termux:X11 应用窗口。

## 顺序是硬要求

**X server 必须先起来，再启动容器。**

Droidspaces 是按**文件**bind-mount `X5` 的。如果容器启动时 socket 还不存在，
它绑到的是占位文件；之后 server 再 `bind()` 会换成一个新的 inode，而容器仍持有旧
inode，`mountinfo` 里表现为：

```text
/data/com.termux/files/usr/tmp/.X11-unix/X5//deleted
```

结果就是容器内 `xdpyinfo: unable to open display ":5"`，而宿主侧 socket 看起来完全正常。
所以不要先起容器再去开 X server。

## 一键启动

`scripts/x11-desktop.sh` 做完整流程，幂等，可从任意状态重复执行。

安装（两个脚本放到设备上，本机已完成）：

```sh
adb push scripts/x11-desktop.sh /data/local/tmp/x11d.sh
adb push scripts/service-d-x11-desktop.sh /data/local/tmp/x11dboot.sh
adb shell su -c 'cp /data/local/tmp/x11d.sh /data/adb/x11-desktop.sh
  cp /data/local/tmp/x11dboot.sh /data/adb/service.d/x11-desktop.sh
  chmod 755 /data/adb/x11-desktop.sh /data/adb/service.d/x11-desktop.sh'
```

运行：

```sh
su -c 'sh /data/adb/x11-desktop.sh'
```

它依次：

1. `chmod 444` 修正 `loader.apk`（见下节）；
2. 检查 `$PREFIX/share/X11/xkb` 是否存在；
3. 以 Termux 的 uid 启动 `termux-x11 :5`（先清掉残留 socket）；
4. 轮询等 `X5` socket 出现；
5. `droidspaces --config=... start`（已在运行则跳过）；
6. 轮询等容器内 `bspc wm -g` 响应，确认桌面起来。

实测从完全干净状态（无 server、无容器）到桌面可用约 **5 秒**。

## 两个必须知道的坑

### 1. `loader.apk` 必须是只读（否则 server 直接 abort）

Android 14+ 拒绝从「调用方 uid 可写」的路径加载 dex。`termux-x11` 包把
`loader.apk` 装在 Termux 自己的数据目录里，默认 `0644`，而运行 server 的正是
Termux 的 uid，于是 `app_process` 在初始化 boot classloader 时崩掉：

```text
Abort message: 'Check failed: system_class_loader != nullptr
java.lang.ExceptionInInitializerError:
Caused by: java.lang.SecurityException: Writable dex file
  '/data/data/com.termux/files/usr/libexec/termux-x11/loader.apk' is not allowed.'
```

修复：

```sh
su -c 'chmod 444 /data/data/com.termux/files/usr/libexec/termux-x11/loader.apk'
```

目录权限可以保持可写，只有 dex 文件本身需要不可写。这个修改在磁盘上持久，
但**重装 `termux-x11` 包后需要重新执行**；启动脚本每次都会带一次，所以不用手动记。

症状对照：如果日志里出现 `Check failed: system_class_loader`，就是这一条。

### 2. `loader.apk` 与 APK 必须同源

loader 会校验已安装的 `com.termux.x11` 的版本/签名，不匹配时报：

```text
Signature verification of target application com.termux.x11 failed.
```

所以 APK 与 `termux-x11` 包要来自**同一次** nightly 构建（nightly 这个 tag 会被移动，
隔几天混用就可能对不上）。本机用的是 APK 与 deb 同时下载的那一组：

```text
termux-x11-universal-debug.apk   sha256 64995746d1887a0e70bc8977d4a9c5ea56e3e00eadd4a6eeab32ae6048ba733a
termux-x11-nightly-1.03.01-0     (deb, 含 loader.apk / termux-x11 / termux-x11-preference)
```

### 3. `sharedUid` 变体在本机不可用

README 建议用 `termux-x11-universal-sharedUid-debug.apk` 来避免后台降频，但它要求
Termux 应用**同签名**（即从 GitHub 装的 Termux）。本机 Termux 是 `0.118.3.64`
（小米安装器装的），签名不匹配：

```text
INSTALL_FAILED_SHARED_USER_INCOMPATIBLE:
Package com.termux.x11 has no signatures that match those in shared user com.termux
```

想用这个变体，需要先卸载 Termux、改装 GitHub 版（会丢 Termux 内的数据）。
不装它也能用，代价是 Termux 退到后台时 CPU 会被限。

## 启动应用

桌面起来后，在容器里带 `DISPLAY=:5` 启动图形程序：

```sh
# 宿主侧下发一条命令
su -c '/data/local/Droidspaces/bin/droidspaces --name=Arch run sh -c \
  "export DISPLAY=:5; unset XAUTHORITY; dbus-launch xfce4-terminal &"'

# 或者进容器交互
su -c '/data/local/Droidspaces/bin/droidspaces --name=Arch enter'
```

容器内 `/root/start-desktop.sh` 是手动启停桌面的入口（正常情况下容器 systemd 已经
把它起来了，重复执行会提示 `bspwm is already running`）。

常用环境变量（`start-desktop.sh` 已设）：

```sh
DISPLAY=:5
unset XAUTHORITY
export XDG_CURRENT_DESKTOP=bspwm
export TERMINAL=xfce4-terminal
```

## 开机自启

`/data/adb/service.d/x11-desktop.sh`（KernelSU 开机脚本）会在
`sys.boot_completed` 之后调用 `/data/adb/x11-desktop.sh`，日志写到
`/data/adb/x11-desktop-boot.log`。

```sh
# 查看
su -c 'cat /data/adb/x11-desktop-boot.log'
# 手动跑一次（幂等）
su -c 'sh /data/adb/x11-desktop.sh'
```

容器自己的 `run_at_boot` 保持 `0`：让 Droidspaces 在开机时先启容器会重新落入
「socket 不存在」的绑定陷阱，必须由脚本先拉起 X server。

## 已验证 / 未验证

已验证：

- 从干净状态一键起桌面，`xdpyinfo` 在容器内报 `1440x2698`；
- 容器内 bspwm / polybar / picom / dunst 全部在运行，窗口可开（`xfce4-terminal` 实测）；
- 截图确认桌面与 Termux:X11 工具栏正常渲染，终端提示符 `[root@QWQ /]#`。

未验证：

- 开机自启路径需要重启才能实测（脚本逻辑本身已手动验证，且幂等）；
- 长时间高负载、外接键鼠、`sxhkd` 热键（容器内当前未装 `sxhkd`）；
- VirGL / PulseAudio 未装（`virgl_test_server_android`、`pulseaudio` 在 Termux 侧缺失，
  Droidspaces 会跳过，不影响 2D 桌面）。

## 与 anland 的关系

[anland](https://github.com/SuperTurtleDev/anland) 是另一条路线：不做「一个远程桌面窗口」，
而是让容器里每个 Wayland/X11 顶层窗口变成**独立的 Android 窗口**（各自进最近任务、
各自有 IME 焦点）。它需要额外三件东西：`anland-awl.zip` 模块、`anland-wayland.apk`、
`anlandx.tar.gz`（容器内会话），再配 `anland-shell` 启动器。

注意 anland 的 `main` 分支是重写后的 6.x，作者自己说明**功能尚不完整**，
想要完整桌面应使用 [`legacy`](https://github.com/SuperTurtleDev/anland/tree/legacy) 分支（5.x）。
5.22 的 Release 资产是 `anland-v5.apk` + `anland-daemon.zip`。

两者可以共存，但没必要同时装。当前这台已经用 Termux:X11 + bspwm 跑通，
先用这个；想要「每个 Linux 应用一个独立 Android 窗口」再考虑 anland。
