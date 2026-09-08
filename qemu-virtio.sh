#!/usr/bin/env bash
# Wrap an existing QEMU boot command, preserving its disk/firmware/CPU settings.
set -euo pipefail
if (( $# == 0 )); then
    echo "Usage: $0 qemu-system-ARCH [existing boot options...]" >&2
    exit 2
fi
: "${VIRTIO_SOCKET:?Set VIRTIO_SOCKET to an absolute path in a private directory}"
if [[ "$VIRTIO_SOCKET" != /* || "$VIRTIO_SOCKET" == *,* ]]; then
    echo 'VIRTIO_SOCKET must be absolute and contain no comma.' >&2
    exit 2
fi
if [[ -e "$VIRTIO_SOCKET" ]]; then
    echo "Socket path already exists: $VIRTIO_SOCKET (use a fresh directory)" >&2
    exit 2
fi
umask 077
exec "$@" \
    -device virtio-serial-pci,id=samplevirtio \
    -chardev "socket,id=samplechannel,path=$VIRTIO_SOCKET,server=on,wait=off" \
    -device virtserialport,bus=samplevirtio.0,chardev=samplechannel,name=org.example.echo
