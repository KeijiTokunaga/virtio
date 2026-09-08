#!/usr/bin/env bash
# 既存VMのディスク・ファームウェア・CPU設定を維持して、virtioポートだけを追加する。
# 使用例: VIRTIO_SOCKET=/tmp/PRIVATE/echo.sock bash ./qemu-virtio.sh qemu-system-... ...
set -euo pipefail
if (( $# == 0 )); then
    echo "Usage: $0 qemu-system-ARCH [existing boot options...]" >&2
    exit 2
fi
: "${VIRTIO_SOCKET:?Set VIRTIO_SOCKET to an absolute path in a private directory}"
# カンマはQEMUのオプション区切りなので、ソケットパス内での使用を拒否する。
if [[ "$VIRTIO_SOCKET" != /* || "$VIRTIO_SOCKET" == *,* ]]; then
    echo 'VIRTIO_SOCKET must be absolute and contain no comma.' >&2
    exit 2
fi
# 別のVMが使っている可能性があるため、既存ソケットは自動削除しない。
if [[ -e "$VIRTIO_SOCKET" ]]; then
    echo "Socket path already exists: $VIRTIO_SOCKET (use a fresh directory)" >&2
    exit 2
fi
umask 077
# execでシェルをQEMUに置き換え、シグナルと終了コードを直接扱えるようにする。
# "$@"で利用者が渡した起動引数を保ち、BEのコントローラー・ソケット・ポートを追加。
# 手動実行用なのでwait=off。ゲストのC++クライアント実行前にホストサービスを接続する。
# 自動デモのrun_demo.pyでは、起動順序を保証するためwait=onを使っている。
exec "$@" \
    -device virtio-serial-pci,id=samplevirtio \
    -chardev "socket,id=samplechannel,path=$VIRTIO_SOCKET,server=on,wait=off" \
    -device virtserialport,bus=samplevirtio.0,chardev=samplechannel,name=org.example.echo
