#!/bin/sh
# Linuxが最初に起動するPID 1。RAM上だけで動く使い捨てゲストの起動処理。
# 初期化 → virtioポート特定 → C++テスト → 電源断までをここで行う。
export PATH=/bin
# BusyBoxの各コマンド名を/binにリンクし、最小限のシェル環境を用意する。
/bin/busybox --install -s /bin
# /devはデバイスノード、/procはプロセス等の情報、/sysはデバイスやドライバーの情報。
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys

fail() {
    # 終了コードだけに頼らず、ホストが読むログへ失敗を記録する。
    # PID 1が単に終了するとkernel panicになるため、電源断後も戻る場合は待機する。
    echo "DEMO_FAIL: $*"
    poweroff -f
    while :; do sleep 1; done
}

echo "=== Linux guest: $(uname -srmo) ==="
mkdir -p /dev/virtio-ports
# 通常のディストリビューションではudevが名前付きリンクを作るが、この最小環境にはない。
# sysfsのnameをQEMUのname=と照合し、番号を決め打ちせず対応するvportを見つける。
# デバイスの認識が完了するまで、最大10回待ち直す。
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
# 実体はカーネル/devtmpfsが作ったキャラクターデバイス。ここではリンクだけを作る。
ln -s "/dev/$port" /dev/virtio-ports/org.example.echo
echo "=== Character device and virtio driver ==="
ls --color=never -l "/dev/$port" /dev/virtio-ports/org.example.echo
cat "/sys/class/virtio-ports/$port/name"
# 接続先がvirtio_console（FE）であることも表示して、通常ファイルとの違いを確認する。
for driver in /sys/bus/virtio/devices/*/driver; do readlink -f "$driver"; done
echo "=== C++ guest round trips ==="
/guest --self-test || fail 'round trip failed'
# 別プロセスで再びopenし、ポートのclose/openをまたいでも往復できることを確認する。
/guest 'Second open from C++ guest' || fail 'second open failed'
# ここまでの両方の実行が成功したときだけ、ホスト側の判定用マーカーを出す。
echo DEMO_PASS
sync
poweroff -f
while :; do sleep 1; done
