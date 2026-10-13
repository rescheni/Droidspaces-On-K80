#!/system/bin/sh
# Boot hook: bring up the Termux:X11 display and the Droidspaces desktop.
#
# Install as /data/adb/service.d/x11-desktop.sh (0755). It only schedules the
# real script, which lives next to it at /data/adb/x11-desktop.sh so that a
# change survives a reboot without touching service.d.
#
# service.d runs while boot is still settling, so wait for boot_completed
# before starting the X server: the Termux data dir and the package list are
# not guaranteed to be ready earlier.
(
	i=0
	while [ "$(getprop sys.boot_completed)" != 1 ] && [ "$i" -lt 180 ]; do
		i=$((i + 1))
		sleep 1
	done
	sleep 5
	sh /data/adb/x11-desktop.sh
) >> /data/adb/x11-desktop-boot.log 2>&1 &
