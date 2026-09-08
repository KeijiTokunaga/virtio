#!/bin/sh
# PID 1 in the disposable RAM-only Linux guest.
export PATH=/bin
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys

fail() {
    echo "DEMO_FAIL: $*"
    poweroff -f
    while :; do sleep 1; done
}

echo "=== Linux guest: $(uname -srmo) ==="
mkdir -p /dev/virtio-ports
# This tiny initramfs has no udev. Reproduce its named symlink from sysfs.
port=
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    for name in /sys/class/virtio-ports/vport*/name; do
        [ -f "$name" ] || continue
        if [ "$(cat "$name")" = org.example.echo ]; then
            port=$(basename "$(dirname "$name")")
            break
        fi
    done
    [ -n "$port" ] && break
    sleep 1
done
[ -n "$port" ] || fail 'virtio port was not discovered'
ln -s "/dev/$port" /dev/virtio-ports/org.example.echo
echo "=== Character device and virtio driver ==="
ls --color=never -l "/dev/$port" /dev/virtio-ports/org.example.echo
cat "/sys/class/virtio-ports/$port/name"
for driver in /sys/bus/virtio/devices/*/driver; do readlink -f "$driver"; done
echo "=== C++ guest round trips ==="
/guest --self-test || fail 'round trip failed'
# A second process verifies that closing/reopening the port works.
/guest 'Second open from C++ guest' || fail 'second open failed'
echo DEMO_PASS
sync
poweroff -f
while :; do sleep 1; done
