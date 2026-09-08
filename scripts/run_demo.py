#!/usr/bin/env python3
"""Run the real virtio end-to-end test; preserve logs and clean up processes."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from prepare_demo import BUILD, ROOT, prepare


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(accelerator):
    qemu_bin = shutil.which("qemu-system-aarch64")
    if not qemu_bin:
        raise RuntimeError("Install QEMU first (macOS: brew install qemu)")
    prepare()
    subprocess.run(["make", "all"], cwd=ROOT, check=True)
    logs = BUILD / "logs"
    logs.mkdir(exist_ok=True)
    qemu = host = None
    # A short private path also fits macOS's sockaddr_un.sun_path.
    with tempfile.TemporaryDirectory(prefix="virtio-", dir="/tmp") as temporary:
        socket_path = str(Path(temporary) / "echo.sock")
        cpu = "cortex-a57" if accelerator == "tcg" else "host"
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
                deadline = time.monotonic() + 15
                while not Path(socket_path).exists():
                    if qemu.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("QEMU did not create its socket; see build/logs/guest.log")
                    time.sleep(0.05)
                host = subprocess.Popen([str(BUILD / "host"), socket_path], stdout=host_log, stderr=subprocess.STDOUT)
                qemu.wait(timeout=90)
                host.wait(timeout=10)
            guest_text = (logs / "guest.log").read_text(errors="replace")
            host_text = (logs / "host.log").read_text(errors="replace")
            print(guest_text)
            print("=== C++ host log ===")
            print(host_text)
            if qemu.returncode or host.returncode or "DEMO_PASS" not in guest_text or "DEMO_FAIL" in guest_text:
                raise RuntimeError("Real virtio test failed; see build/logs/")
            if host_text.count("received ") != 5:
                raise RuntimeError("Host did not receive all five requests")
            print("PASS: 5 real virtio round trips, including a second device open.")
        finally:
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
