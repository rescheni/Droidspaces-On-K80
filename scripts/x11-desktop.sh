#!/system/bin/sh
# Bring up the Termux:X11 display, then the Droidspaces container desktop.
#
# Ordering is not optional. Droidspaces bind-mounts the X5 socket FILE when the
# container starts; if the socket does not exist yet it binds a placeholder and
# the container keeps a dead inode (".../X5//deleted" in mountinfo), so the
# guest cannot open the display. Start the server first, wait for the socket,
# and only then start the container.
set -u

C=Arch
P=/data/data/com.termux/files/usr
DS=/data/local/Droidspaces/bin/droidspaces
CFG=/data/local/Droidspaces/Containers/$C/container.config
TERMUX_UID=$(awk '$1=="com.termux"{print $2}' /data/system/packages.list)
SOCK=$P/tmp/.X11-unix/X5

echo "== termux uid: $TERMUX_UID"

# Android 14+ refuses to load a dex that the calling uid can write. The
# termux-x11 companion package installs loader.apk as 0644 inside Termux's own
# data dir, so app_process aborts with:
#   SecurityException: Writable dex file '.../loader.apk' is not allowed
# Make it read-only; the directory may stay writable.
chmod 444 "$P/libexec/termux-x11/loader.apk" 2>/dev/null

# xkb data is a runtime dependency of the loader (compiled into the x11 repo).
[ -e "$P/share/X11/xkb" ] || echo "!! missing $P/share/X11/xkb (install xkeyboard-config)"

xserver_running() {
	[ -S "$SOCK" ] || return 1
	pidof -s "termux-x11 com.termux.x11 :5" >/dev/null 2>&1 ||
		pgrep -f "termux-x11 com.termux.x11 :5" >/dev/null 2>&1
}

echo "== starting the X server on :5"
if xserver_running; then
	echo "   already running"
else
	rm -f "$SOCK"
	# Background the su itself; a nohup inside `su -c` does not survive the
	# su shell exiting and can fail to resolve the launcher.
	su "$TERMUX_UID" -c "export PREFIX=$P HOME=/data/data/com.termux/files/home \
PATH=$P/bin TMPDIR=$P/tmp XKB_CONFIG_ROOT=$P/share/X11/xkb; \
cd \$HOME && exec termux-x11 :5" > "$P/tmp/tx11.log" 2>&1 &
fi

echo "== waiting for $SOCK"
i=0
while [ "$i" -lt 30 ]; do
	[ -S "$SOCK" ] && break
	i=$((i + 1))
	sleep 1
done
if [ ! -S "$SOCK" ]; then
	echo "!! X socket never appeared; see $P/tmp/tx11.log"
	exit 1
fi
echo "   socket ready"

echo "== starting the container"
if "$DS" --name=$C pid >/dev/null 2>&1; then
	echo "   container already running"
else
	"$DS" --config="$CFG" start
fi

echo "== waiting for the desktop"
i=0
while [ "$i" -lt 30 ]; do
	if "$DS" --name=$C run sh -c 'DISPLAY=:5 bspc wm -g' >/dev/null 2>&1; then
		echo "   desktop is up on :5"
		break
	fi
	i=$((i + 1))
	sleep 1
done

echo "== open the Termux:X11 app to see it"
