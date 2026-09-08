#!/usr/bin/env python3
"""Build an ARM64 Linux initramfs without Docker, root, or a guest OS install."""
import gzip
import hashlib
import os
from pathlib import Path
import platform
import stat
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache"
BUILD = ROOT / "build"
ALPINE = "https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/"
KERNEL = "linux-virt-6.18.49-r0.apk"
BUSYBOX = "busybox-static-1.37.0-r30.apk"
# Alpine artifacts were downloaded over HTTPS and pinned by SHA-256.
# Zig checksums come from https://ziglang.org/download/index.json (0.15.2).
HASHES = {
    KERNEL: "b5dcf3f5e4fd19a5413bdc858e746fec5ac5256e5db8cba94f808c9c6382b877",
    BUSYBOX: "44c9abdfb970f398fa72c8382fe2d8808eea16beaf82daf6ac708b92f1b8659e",
    "aarch64-macos": "3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b",
    "x86_64-macos": "375b6909fc1495d16fc2c7db9538f707456bfc3373b14ee83fdd3e22b3d43f7f",
    "aarch64-linux": "958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f",
    "x86_64-linux": "02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239",
}


def download(url, expected):
    path = CACHE / "downloads" / url.rsplit("/", 1)[-1]
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".part")
        print(f"Downloading {url}", flush=True)
        subprocess.run(["curl", "-fL", "--retry", "2", "--connect-timeout", "15",
                        "--max-time", "600", url, "-o", str(temporary)], check=True)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
            temporary.unlink()
            raise RuntimeError(f"SHA-256 mismatch: {path.name}")
        temporary.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise RuntimeError(f"SHA-256 mismatch in cached file: {path}")
    return path


def apk_file(archive, name):
    # APK v2 consists of concatenated gzip/tar sections.
    with tarfile.open(archive, "r:gz", ignore_zeros=True) as package:
        member = package.extractfile(name)
        if member is None:
            raise RuntimeError(f"Missing {name} in {archive}")
        return member.read()


def initramfs(entries):
    """Encode Linux newc cpio directly; device nodes do not require host root."""
    output = bytearray()
    for inode, (name, mode, data, major, minor) in enumerate(entries, 1):
        encoded = name.encode() + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0)
        output += b"070701" + "".join(f"{value:08x}" for value in fields).encode()
        output += encoded
        output += b"\0" * (-len(output) % 4)
        output += data
        output += b"\0" * (-len(output) % 4)
    return gzip.compress(output, mtime=0)


def prepare():
    BUILD.mkdir(exist_ok=True)
    machine = {"arm64": "aarch64", "aarch64": "aarch64", "x86_64": "x86_64"}.get(platform.machine())
    system = {"Darwin": "macos", "Linux": "linux"}.get(platform.system())
    host = f"{machine}-{system}"
    if host not in HASHES:
        raise RuntimeError("Demo supports macOS/Linux on ARM64 or x86_64")
    zig_name = f"zig-{host}-0.15.2"
    zig_archive = download(f"https://ziglang.org/download/0.15.2/{zig_name}.tar.xz", HASHES[host])
    zig_dir = CACHE / zig_name
    if not (zig_dir / "zig").exists():
        with tarfile.open(zig_archive) as archive:
            archive.extractall(CACHE, filter="data")
    kernel_package = download(ALPINE + KERNEL, HASHES[KERNEL])
    busybox_package = download(ALPINE + BUSYBOX, HASHES[BUSYBOX])

    guest = BUILD / "guest-linux-aarch64"
    sources = [ROOT / "src/guest.cpp", ROOT / "src/transport.hpp"]
    if not guest.exists() or any(p.stat().st_mtime > guest.stat().st_mtime for p in sources):
        print("Cross-compiling C++ guest for aarch64-linux-musl (first build may take a minute)...", flush=True)
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(CACHE / "zig-global"),
                   ZIG_LOCAL_CACHE_DIR=str(CACHE / "zig-local"))
        subprocess.run([str(zig_dir / "zig"), "c++", "-target", "aarch64-linux-musl", "-static",
                        "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Isrc",
                        "src/guest.cpp", "-o", str(guest)], cwd=ROOT, env=env, check=True)

    # virtio_console and virtio_pci are built into this pinned kernel.
    config = apk_file(kernel_package, "boot/config-6.18.49-0-virt").decode()
    for setting in ("CONFIG_VIRTIO_CONSOLE=y", "CONFIG_VIRTIO_PCI=y", "CONFIG_DEVTMPFS=y"):
        if setting not in config.splitlines():
            raise RuntimeError(f"Kernel is missing {setting}")
    (BUILD / "vmlinuz-virt").write_bytes(apk_file(kernel_package, "boot/vmlinuz-virt"))
    entries = [(d, stat.S_IFDIR | 0o755, b"", 0, 0) for d in ("bin", "dev", "proc", "sys")]
    entries += [
        ("bin/busybox", stat.S_IFREG | 0o755, apk_file(busybox_package, "bin/busybox.static"), 0, 0),
        ("bin/sh", stat.S_IFLNK | 0o777, b"busybox", 0, 0),
        ("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
        ("init", stat.S_IFREG | 0o755, (ROOT / "scripts/guest-init.sh").read_bytes(), 0, 0),
        ("guest", stat.S_IFREG | 0o755, guest.read_bytes(), 0, 0),
        ("TRAILER!!!", 0, b"", 0, 0),
    ]
    (BUILD / "initramfs.cpio.gz").write_bytes(initramfs(entries))
    print("Prepared kernel + RAM-only initramfs.", flush=True)


if __name__ == "__main__":
    prepare()
