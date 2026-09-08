#!/usr/bin/env python3
"""QEMUとC++ホストを起動し、本物のvirtio経路で往復通信を検証する。

QEMUがソケットを作る → C++ホストが接続 → Linuxゲストが起動して要求を送る、の順序。
Pythonはプロセスを管理するだけで、echoのデータ経路には入らない。
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from prepare_demo import BUILD, ROOT, prepare


def stop(process):
    """自分が起動したプロセスだけを終了し、終了待ちまで行う。"""
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            # 通常の終了要求で止まらない場合だけ強制終了する。
            process.kill()
            process.wait()


def run(accelerator):
    """成功マーカーと終了コードを検証し、成功・失敗どちらでも後片付けする。"""
    qemu_bin = shutil.which("qemu-system-aarch64")
    if not qemu_bin:
        raise RuntimeError("Install QEMU first (macOS: brew install qemu)")
    prepare()
    subprocess.run(["make", "all"], cwd=ROOT, check=True)
    logs = BUILD / "logs"
    logs.mkdir(exist_ok=True)
    qemu = host = None
    # Unixソケットのパス長制限に収まる短い専用ディレクトリを使う。
    # ブロックを抜けると削除されるため、前回のソケットが次回に残らない。
    with tempfile.TemporaryDirectory(prefix="virtio-", dir="/tmp") as temporary:
        socket_path = str(Path(temporary) / "echo.sock")
        # TCGはARM CPUをエミュレーションする。HVF/KVMはホストのARM CPUを使う。
        # 実行方式を変えても、virtioのBEはどちらもQEMUが担当する。
        cpu = "cortex-a57" if accelerator == "tcg" else "host"
        # -kernel/-initrd: ディスクやブートローダーを介さずLinuxを起動する。
        # -serial stdioとconsole=ttyAMA0: 起動ログ用の通常のUART。echo用virtioとは別経路。
        # -nic none: ゲストのネットワークなしでも通信できることを示す。
        # virtio-serial-pci: PCI上にコントローラーを公開するQEMU側のデバイス。
        # chardev: BEの先のUnixソケット。wait=onでホスト接続前のゲスト起動を防ぐ。
        # virtserialport: そのソケットを、org.example.echoというゲストポートに結び付ける。
        command = [qemu_bin, "-machine", "virt", "-cpu", cpu, "-accel", accelerator,
                   "-m", "256", "-display", "none", "-monitor", "none", "-serial", "stdio",
                   "-nic", "none", "-no-reboot", "-kernel", str(BUILD / "vmlinuz-virt"),
                   "-initrd", str(BUILD / "initramfs.cpio.gz"),
                   "-append", "console=ttyAMA0 loglevel=4 panic=1",
                   "-device", "virtio-serial-pci,id=samplevirtio",
                   "-chardev", f"socket,id=samplechannel,path={socket_path},server=on,wait=on",
                   "-device", "virtserialport,bus=samplevirtio.0,chardev=samplechannel,name=org.example.echo"]
        print(f"Starting QEMU/{accelerator.upper()} (ARM64 guest, no disk or network)...", flush=True)
        try:
            with (logs / "guest.log").open("w") as guest_log, (logs / "host.log").open("w") as host_log:
                qemu = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=guest_log, stderr=subprocess.STDOUT)
                # 固定秒数だけ寝るのではなく、QEMUがソケットを作った時点で接続する。
                # この時点ではwait=onにより、まだゲストは実行を開始していない。
                deadline = time.monotonic() + 15
                while not Path(socket_path).exists():
                    if qemu.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("QEMU did not create its socket; see build/logs/guest.log")
                    time.sleep(0.05)
                host = subprocess.Popen([str(BUILD / "host"), socket_path], stdout=host_log, stderr=subprocess.STDOUT)
                # ゲストの/initはテスト後に電源を切る。QEMU終了後はソケットも閉じ、
                # ホスト側のreceiveがEOFを検出して終了する。
                qemu.wait(timeout=90)
                host.wait(timeout=10)
            guest_text = (logs / "guest.log").read_text(errors="replace")
            host_text = (logs / "host.log").read_text(errors="replace")
            print(guest_text)
            print("=== C++ host log ===")
            print(host_text)
            # VMが正常終了しただけでは通信成功とは限らないため、ゲストの成功マーカーと
            # ホストの受信件数も照合する（自己テスト4要求＋再オープン後の1要求）。
            if qemu.returncode or host.returncode or "DEMO_PASS" not in guest_text or "DEMO_FAIL" in guest_text:
                raise RuntimeError("Real virtio test failed; see build/logs/")
            if host_text.count("received ") != 5:
                raise RuntimeError("Host did not receive all five requests")
            print("PASS: 5 real virtio round trips, including a second device open.")
        finally:
            # 起動途中の失敗、待ち時間超過、Ctrl-Cの場合も子プロセスを残さない。
            stop(host)
            stop(qemu)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", choices=("tcg", "hvf", "kvm"), default="tcg",
                        help="hvf: ARM64 macOS; kvm: ARM64 Linux; default: portable TCG")
    args = parser.parse_args()
    try:
        run(args.accel)
    except (Exception, KeyboardInterrupt) as error:
        raise SystemExit(f"Demo failed: {error}. Logs: {BUILD / 'logs'}")
