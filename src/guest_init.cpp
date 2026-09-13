// Linuxが最初に実行する/init（PID 1）。ARM64 Linux向けに静的リンクする。
// シェルやBusyBoxを使わず、デバイス確認・C++アプリの実行・電源断まで行う。
#include "transport.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
namespace {
[[noreturn]] void power_down() {
    ::sync();
    ::reboot(RB_POWER_OFF);
    // PID 1は通常のプロセスのようにreturnしない。電源断失敗時は待機する。
    ::perror("poweroff");
    for (;;) ::pause();
}

void run_guest(const char* argument) {
    const pid_t pid = ::fork();
    if (pid < 0) demo::system_error("fork");
    if (pid == 0) {
        ::execl("/guest", "/guest", argument, static_cast<char*>(nullptr));
        ::perror("exec /guest");
        ::_exit(127);
    }
    int status;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) demo::system_error("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("C++ guest round trip failed");
}
}

int main() {
    if (::getpid() != 1) {
        std::cerr << "guest_init must run as PID 1 inside the disposable Linux guest.\n";
        return 1;
    }
    try {
        // devtmpfsが本物のデバイスノードを提供し、sysfsで対応するドライバーを確認できる。
        for (const auto& pair : {std::pair{"devtmpfs", "/dev"}, {"proc", "/proc"}, {"sysfs", "/sys"}}) {
            if (::mount(pair.first, pair.second, pair.first, 0, nullptr) < 0) demo::system_error("mount");
        }
        struct utsname kernel_info{};
        if (::uname(&kernel_info) < 0) demo::system_error("uname");
        std::cout << "=== C++ init: Linux " << kernel_info.release << ' ' << kernel_info.machine << " ===" << std::endl;
        fs::path port;
        for (int attempt = 0; attempt < 100 && port.empty(); ++attempt) {
            // PCIの列挙・ドライバー初期化は非同期なので、認識されるまで最大約10秒待つ。
            if (fs::exists("/sys/class/virtio-ports")) {
                for (const auto& entry : fs::directory_iterator("/sys/class/virtio-ports")) {
                    std::ifstream name(entry.path() / "name");
                    std::string value;
                    std::getline(name, value);
                    if (value == "org.example.echo") { port = entry.path(); break; }
                }
            }
            if (port.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (port.empty()) throw std::runtime_error("virtio port was not discovered");
        const fs::path device = fs::path("/dev") / port.filename();
        struct stat info{};
        if (::stat(device.c_str(), &info) < 0) demo::system_error("stat virtio port");
        if (!S_ISCHR(info.st_mode)) throw std::runtime_error("virtio port is not a character device");
        const auto driver = fs::canonical(port / "device/driver");
        if (driver.filename() != "virtio_console") throw std::runtime_error("Unexpected virtio driver");
        // この最小環境にはudevがないため、sysfsのnameを確認して名前付きリンクを作る。
        fs::create_directories("/dev/virtio-ports");
        fs::create_symlink(device, "/dev/virtio-ports/org.example.echo");
        std::cout << "/dev/virtio-ports/org.example.echo -> " << device.string()
                  << "\ncharacter device " << major(info.st_rdev) << ':' << minor(info.st_rdev)
                  << "\ndriver: " << driver.string() << std::endl;
        run_guest("--self-test");
        // 別プロセスで開き直すことで、ポートのclose/openをまたいだ通信も検証する。
        run_guest("Second open from C++ guest");
        std::cout << "DEMO_PASS" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "DEMO_FAIL: " << error.what() << std::endl;
    }
    power_down();
}
