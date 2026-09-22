// ホスト上で動くデモ実行プログラム。
// 配布物取得 → Linux向けC++ビルド → initramfs作成 → QEMU起動 → 結果検証。
// virtioのデータ経路には入らず、通信はguest.cpp / QEMU / host.cppが担当する。
#include "transport.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int signal) { interrupted = signal; }

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read: " + path.string());
    std::ostringstream data;
    data << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Read failed: " + path.string());
    return data.str();
}

void write_file(const fs::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) throw std::runtime_error("Write failed: " + path.string());
}

// シェルへ文字列を渡さず、引数配列をexecvpへ渡す。空白を含むパスも扱える。
// 子ごとにプロセスグループを作り、異常終了時はコンパイラ等の孫プロセスも回収する。
class Process {
public:
    explicit Process(const std::vector<std::string>& args, const fs::path& output = {}, bool merge_errors = false) {
        if (args.empty()) throw std::invalid_argument("empty command");
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        pid_ = ::fork();
        if (pid_ < 0) demo::system_error("fork");
        if (pid_ == 0) {
            ::signal(SIGINT, SIG_DFL);
            ::signal(SIGTERM, SIG_DFL);
            if (::setpgid(0, 0) < 0) child_error("setpgid");
            const int input = ::open("/dev/null", O_RDONLY);
            if (input < 0 || ::dup2(input, STDIN_FILENO) < 0) child_error("stdin");
            if (input > STDERR_FILENO) ::close(input);
            if (!output.empty()) {
                const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) child_error("stdout");
                if (merge_errors && ::dup2(fd, STDERR_FILENO) < 0) child_error("stderr");
                if (fd > STDERR_FILENO) ::close(fd);
            }
            ::execvp(argv[0], argv.data());
            child_error(argv[0]);
        }
        // 子がexec前にグループを作る。親側も試み、終了処理との競合を小さくする。
        ::setpgid(pid_, pid_);
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() {
        // 正常にwait済みのPIDへはシグナルを送らない（OSで再利用され得る）。
        if (done_) return;
        ::kill(-pid_, SIGTERM);
        for (int i = 0; !finished() && i < 50; ++i) std::this_thread::sleep_for(100ms);
        if (!finished()) {
            ::kill(-pid_, SIGKILL);
            while (::waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {}
        }
    }
    bool finished() noexcept {
        if (done_) return true;
        const auto result = ::waitpid(pid_, &status_, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) done_ = true;
        return done_;
    }
    int wait(int seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!finished()) {
            if (interrupted) throw std::runtime_error("Interrupted");
            if (std::chrono::steady_clock::now() >= deadline) throw demo::Timeout();
            std::this_thread::sleep_for(20ms);
        }
        return WIFEXITED(status_) ? WEXITSTATUS(status_) : 128 + WTERMSIG(status_);
    }
private:
    [[noreturn]] static void child_error(const char* operation) {
        ::perror(operation);
        ::_exit(127);
    }
    pid_t pid_ = -1;
    int status_ = 0;
    bool done_ = false;
};

void command(const std::vector<std::string>& args, const fs::path& output = {}) {
    Process process(args, output);
    if (process.wait(600) != 0) throw std::runtime_error("Command failed: " + args.front());
}

// SHA-256、HTTPS、tarの実装はOSのコマンドに任せる。独自の暗号・展開処理は持たない。
bool checksum(const fs::path& path, const std::string& expected) {
    const auto result = path.string() + ".sha256";
#ifdef __APPLE__
    command({"shasum", "-a", "256", path.string()}, result);
#else
    command({"sha256sum", path.string()}, result);
#endif
    const auto actual = read_file(result);
    fs::remove(result);
    return actual.substr(0, 64) == expected;
}

fs::path download(const std::string& url, const std::string& hash) {
    const fs::path path = fs::path(".cache/downloads") / url.substr(url.find_last_of('/') + 1);
    fs::create_directories(path.parent_path());
    if (!fs::exists(path)) {
        const auto temporary = path.string() + ".part";
        std::cout << "Downloading " << url << std::endl;
        command({"curl", "-fL", "--retry", "2", "--connect-timeout", "15", "--max-time", "600", url, "-o", temporary});
        if (!checksum(temporary, hash)) {
            fs::remove(temporary);
            throw std::runtime_error("SHA-256 mismatch: " + path.string());
        }
        fs::rename(temporary, path);
    } else if (!checksum(path, hash)) {
        throw std::runtime_error("SHA-256 mismatch: " + path.string());
    }
    return path;
}

// Linux newc cpioの1エントリー。デバイスノードをホスト上でmknodする必要はない。
// 本文は4バイト境界に揃え、ヘッダーの各値は8桁の16進ASCIIで記録する。
void cpio_entry(std::string& archive, unsigned inode, const std::string& name,
                unsigned mode, const std::string& data = {}, unsigned major = 0, unsigned minor = 0) {
    if (data.size() > 0xffffffffu) throw std::runtime_error("cpio entry too large");
    archive += "070701";
    const unsigned fields[] = {inode, mode, 0, 0, 1, 0, static_cast<unsigned>(data.size()),
                               0, 0, major, minor, static_cast<unsigned>(name.size() + 1), 0};
    for (unsigned value : fields) {
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08x", value);
        archive += hex;
    }
    archive += name;
    archive += '\0';
    archive.append((4 - archive.size() % 4) % 4, '\0');
    archive += data;
    archive.append((4 - archive.size() % 4) % 4, '\0');
}

void prepare() {
    struct utsname host{};
    if (::uname(&host) < 0) demo::system_error("uname");
    std::string arch = host.machine;
    if (arch == "arm64") arch = "aarch64";
    if (std::string(host.sysname) != "Darwin" && std::string(host.sysname) != "Linux")
        throw std::runtime_error("Demo supports macOS and Linux");
    const std::string os = std::string(host.sysname) == "Darwin" ? "macos" : "linux";
    const auto platform = arch + "-" + os;
    // Zig公式0.15.2のチェックサム。コンパイラはホスト向け、出力は常にARM64 Linux。
    std::string hash;
    if (platform == "aarch64-macos") hash = "3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b";
    else if (platform == "x86_64-macos") hash = "375b6909fc1495d16fc2c7db9538f707456bfc3373b14ee83fdd3e22b3d43f7f";
    else if (platform == "aarch64-linux") hash = "958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f";
    else if (platform == "x86_64-linux") hash = "02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239";
    else throw std::runtime_error("Demo supports ARM64/x86_64 macOS and Linux");
    fs::create_directories("build");
    const auto zig_name = "zig-" + platform + "-0.15.2";
    const auto zig_archive = download("https://ziglang.org/download/0.15.2/" + zig_name + ".tar.xz", hash);
    const fs::path zig = fs::path(".cache") / zig_name / "zig";
    if (!fs::exists(zig)) command({"tar", "-xJf", zig_archive.string(), "-C", ".cache"});
    const auto kernel = download("https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/linux-virt-6.18.50-r0.apk",
                                 "6ac5a9819500df655592b8b15b4e79dc14c946ee46f0450425faad9e3901fdc3");
    // APKは複数のtar区画を連結している。--ignore-zerosで区画をまたいで読む。
    command({"tar", "--ignore-zeros", "-xOf", kernel.string(), "boot/config-6.18.50-0-virt"}, "build/kernel.config");
    const auto config = read_file("build/kernel.config");
    for (const auto* setting : {"CONFIG_VIRTIO_CONSOLE=y", "CONFIG_VIRTIO_PCI=y", "CONFIG_DEVTMPFS=y"}) {
        if (("\n" + config).find("\n" + std::string(setting) + "\n") == std::string::npos)
            throw std::runtime_error("Kernel is missing " + std::string(setting));
    }
    command({"tar", "--ignore-zeros", "-xOf", kernel.string(), "boot/vmlinuz-virt"}, "build/vmlinuz-virt");
    if (::setenv("ZIG_GLOBAL_CACHE_DIR", fs::absolute(".cache/zig-global").c_str(), 1) < 0 ||
        ::setenv("ZIG_LOCAL_CACHE_DIR", fs::absolute(".cache/zig-local").c_str(), 1) < 0)
        demo::system_error("setenv");
    std::cout << "Cross-compiling guest and init for ARM64 Linux..." << std::endl;
    for (const auto* source : {"guest", "guest_init"}) {
        // 静的リンクにより、ゲストには共有ライブラリもシェルも不要になる。
        command({zig.string(), "c++", "-target", "aarch64-linux-musl", "-static", "-std=c++17", "-O2",
                 "-Wall", "-Wextra", "-Wpedantic", "-Isrc", "src/" + std::string(source) + ".cpp",
                 "-o", "build/" + std::string(source) + "-linux-aarch64"});
    }
    std::string archive;
    unsigned inode = 1;
    for (const auto* dir : {"dev", "proc", "sys"}) cpio_entry(archive, inode++, dir, S_IFDIR | 0755);
    cpio_entry(archive, inode++, "dev/console", S_IFCHR | 0600, {}, 5, 1);
    cpio_entry(archive, inode++, "init", S_IFREG | 0755, read_file("build/guest_init-linux-aarch64"));
    cpio_entry(archive, inode++, "guest", S_IFREG | 0755, read_file("build/guest-linux-aarch64"));
    cpio_entry(archive, inode++, "TRAILER!!!", 0);
    // Linuxは非圧縮のcpioも読めるため、gzipへの依存は不要。
    write_file("build/initramfs.cpio", archive);
    std::cout << "Prepared C++ initramfs (no Python or BusyBox)." << std::endl;
}

// ソケットパスを短く保ち、他のプロセスのファイルと衝突しない専用ディレクトリを使う。
class SocketDirectory {
public:
    SocketDirectory() {
        char path[] = "/tmp/virtio-cpp-XXXXXX";
        if (!::mkdtemp(path)) demo::system_error("mkdtemp");
        path_ = path;
    }
    ~SocketDirectory() { std::error_code error; fs::remove_all(path_, error); }
    std::string socket() const { return (path_ / "echo.sock").string(); }
private:
    fs::path path_;
};

void run_demo(const std::string& accelerator) {
    prepare();
    fs::create_directories("build/logs");
    SocketDirectory directory;
    const auto socket = directory.socket();
    std::cout << "Starting QEMU/" << accelerator << "..." << std::endl;
    // virtioのBEはQEMU。wait=onでホスト接続後に初めてLinuxを走らせる。
    Process qemu({"qemu-system-aarch64", "-machine", "virt", "-cpu", accelerator == "tcg" ? "cortex-a57" : "host",
                  "-accel", accelerator, "-m", "256", "-display", "none", "-monitor", "none", "-serial", "stdio",
                  "-nic", "none", "-no-reboot", "-kernel", "build/vmlinuz-virt", "-initrd", "build/initramfs.cpio",
                  "-append", "console=ttyAMA0 loglevel=4 panic=1", "-device", "virtio-serial-pci,id=samplevirtio",
                  "-chardev", "socket,id=samplechannel,path=" + socket + ",server=on,wait=on",
                  "-device", "virtserialport,bus=samplevirtio.0,chardev=samplechannel,name=org.example.echo"},
                 "build/logs/guest.log", true);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!fs::exists(socket)) {
        if (interrupted) throw std::runtime_error("Interrupted");
        if (qemu.finished() || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("QEMU did not create its socket; see build/logs/guest.log");
        std::this_thread::sleep_for(20ms);
    }
    Process host({"./build/host", socket}, "build/logs/host.log", true);
    const int qemu_status = qemu.wait(90);
    const int host_status = host.wait(10);
    const auto guest_log = read_file("build/logs/guest.log");
    const auto host_log = read_file("build/logs/host.log");
    std::cout << guest_log << "\n=== C++ host log ===\n" << host_log;
    std::size_t received = 0;
    for (std::size_t offset = 0; (offset = host_log.find("received ", offset)) != std::string::npos; offset += 9) ++received;
    // 電源断だけでは成功扱いにせず、マーカーとホストの5要求も確認する。
    if (qemu_status || host_status || received != 5 || guest_log.find("DEMO_PASS") == std::string::npos ||
        guest_log.find("DEMO_FAIL") != std::string::npos)
        throw std::runtime_error("Real virtio test failed; see build/logs/");
    std::cout << "PASS: 5 real virtio round trips, including a second device open." << std::endl;
}

// VMもネットワークも不要な、準備・起動処理の最小チェック。
// 実際のLinuxによるcpio展開と/init実行はmake demoで別途検証する。
void self_test() {
    auto check = [](bool condition) {
        if (!condition) throw std::runtime_error("demo self-test failed");
    };
    std::string archive;
    cpio_entry(archive, 1, "sample", S_IFREG | 0755, std::string("a\0b", 3));
    check(archive.size() == 124 && archive.substr(0, 6) == "070701");
    check(archive.substr(54, 8) == "00000003" && archive.substr(94, 8) == "00000007");
    check(archive.substr(120, 3) == std::string("a\0b", 3));
    SocketDirectory directory;
    const auto path = directory.socket() + " binary file";
    write_file(path, "abc");
    check(read_file(path) == "abc");
    check(checksum(path, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    check(!checksum(path, std::string(64, '0')));
    // 固定コマンドで終了コードと期限切れを確認する。外部入力はシェルへ渡さない。
    { Process child({"sh", "-c", "exit 7"}); check(child.wait(5) == 7); }
    bool timed_out = false;
    try { Process child({"sleep", "5"}); child.wait(0); }
    catch (const demo::Timeout&) { timed_out = true; }
    check(timed_out);
    std::cout << "PASS: demo self-test (cpio, checksum, paths, exit status, timeout)." << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    try {
        std::string accelerator = "tcg";
        bool prepare_only = false;
        bool test_only = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--prepare-only") prepare_only = true;
            else if (arg == "--self-test") test_only = true;
            else if (arg == "--accel" && i + 1 < argc) accelerator = argv[++i];
            else if (arg.rfind("--accel=", 0) == 0) accelerator = arg.substr(8);
            else if (arg == "--help") {
                std::cout << "Usage: ./build/demo [--accel=tcg|hvf|kvm] [--prepare-only] [--self-test]\nRun from the repository root.\n";
                return 0;
            } else throw std::invalid_argument("Invalid arguments; see --help");
        }
        if (accelerator != "tcg" && accelerator != "hvf" && accelerator != "kvm")
            throw std::invalid_argument("Unknown accelerator");
        if (test_only) { self_test(); return 0; }
        if (!fs::exists("src/guest.cpp") || !fs::exists("src/guest_init.cpp"))
            throw std::runtime_error("Run from the repository root");
        if (prepare_only) prepare();
        else run_demo(accelerator);
    } catch (const std::exception& error) {
        std::cerr << "Demo failed: " << error.what() << '\n';
        return 1;
    }
}
