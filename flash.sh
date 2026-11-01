#!/bin/sh
# Flash usb.img onto a USB stick.
#
#   make flash USB=/dev/sdX          (re-runs itself through sudo if needed)
#   make flash USB=/dev/sdX FORCE=1  (skip the USB/removable device check)
#
# Refuses to touch partitioned devices or devices with mounted filesystems,
# prints a summary and needs "yes" before writing anything.
set -eu

# Third argument (or IMG= in the environment) selects the image.  Passing it
# as an argument keeps it alive across the sudo re-exec below.
IMG=${3:-${IMG:-usb.img}}
dev=${1:-}
force=${2:-}

if [ -z "$dev" ]; then
	echo "usage: make flash USB=/dev/sdX [FORCE=1] [IMAGE]" >&2
	exit 1
fi

if [ ! -f "$IMG" ]; then
	echo "flash: $IMG not found (run 'make usb.img' first)" >&2
	exit 1
fi

if [ ! -b "$dev" ]; then
	echo "flash: $dev: not a block device" >&2
	exit 1
fi

command -v lsblk >/dev/null 2>&1 || { echo "flash: lsblk not found" >&2; exit 1; }

dev_type=$(lsblk -ndo TYPE "$dev" | tr -d ' ')
rm_flag=$(lsblk -ndo RM "$dev" | tr -d ' ')
tran=$(lsblk -ndo TRAN "$dev" | tr -d ' ')
model=$(lsblk -ndo MODEL "$dev" | tr -s ' ' | sed 's/^ *//;s/ *$//')
dev_size=$(lsblk -bndo SIZE "$dev" | tr -d ' ')
img_size=$(stat -c%s "$IMG")

case "$dev_type" in
part)
	echo "flash: $dev is a partition; pass the whole disk (e.g. /dev/sdb)" >&2
	exit 1
	;;
esac

if [ "$force" != "1" ] && [ "$tran" != "usb" ] && [ "$rm_flag" != "1" ]; then
	echo "flash: $dev does not look like a USB/removable device (TRAN=$tran RM=$rm_flag)" >&2
	echo "flash: pass FORCE=1 if you really mean it" >&2
	exit 1
fi

mounted=$(lsblk -nro MOUNTPOINT "$dev" | sed '/^$/d')
if [ -n "$mounted" ]; then
	echo "flash: $dev has mounted filesystems:" >&2
	echo "$mounted" | sed 's/^/  /' >&2
	echo "flash: unmount them first (e.g. sudo umount $dev?*)" >&2
	exit 1
fi

if [ "$dev_size" -eq 0 ]; then
	echo "flash: $dev reports 0 bytes - the kernel sees no medium" >&2
	echo "flash: the stick probably dropped off the bus; replug it into a direct" >&2
	echo "flash: port (not a hub) and watch 'lsblk' / 'journalctl -k -f'" >&2
	exit 1
fi

if [ "$dev_size" -lt "$img_size" ]; then
	echo "flash: $dev ($dev_size bytes) is smaller than $IMG ($img_size bytes)" >&2
	exit 1
fi

# dd needs root; re-exec through sudo if we are not root yet.
if [ "$(id -u)" -ne 0 ]; then
	if command -v sudo >/dev/null 2>&1; then
		echo "flash: re-running with sudo..."
		exec sudo "$0" "$@"
	fi
	echo "flash: must run as root (no sudo found)" >&2
	exit 1
fi

echo "About to write $IMG ($img_size bytes) to:"
echo "  device: $dev"
echo "  size:   $dev_size bytes"
echo "  model:  ${model:-unknown}"
echo "  bus:    ${tran:-unknown} (removable=$rm_flag)"
printf 'ALL DATA ON %s WILL BE DESTROYED. Type "yes" to continue: ' "$dev"
read -r answer
if [ "$answer" != "yes" ]; then
	echo "flash: aborted"
	exit 1
fi

echo "flash: writing..."
dd if="$IMG" of="$dev" bs=4M conv=fsync status=progress

echo "flash: syncing..."
sync

printf 'flash: verifying... '
if head -c "$img_size" "$dev" | cmp -s - "$IMG"; then
	echo "OK"
else
	echo "MISMATCH" >&2
	exit 1
fi

echo "flash: done. Safe to remove $dev."
