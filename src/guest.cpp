#include "transport.hpp"
#include <csignal>
#include <iostream>
#include <sys/stat.h>
#include <vector>

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        std::string device = "/dev/virtio-ports/org.example.echo";
        std::string message = "Hello from C++ guest!";
        bool self_test = false;
        bool have_message = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--device" && i + 1 < argc) device = argv[++i];
            else if (arg == "--self-test") self_test = true;
            else if (arg == "--help") {
                std::cout << "Usage: guest [--device PATH] [--self-test] [MESSAGE]\n";
                return 0;
            } else if (arg.rfind("--", 0) == 0 || have_message) {
                throw std::invalid_argument("invalid arguments; see --help");
            } else { message = arg; have_message = true; }
        }

        // The guest opens a Linux CHARACTER DEVICE. No socket API is used here.
        demo::Fd fd(::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
        struct stat info{};
        if (::fstat(fd.get(), &info) < 0) demo::system_error("fstat");
        if (!S_ISCHR(info.st_mode)) throw std::runtime_error("expected a character device");
        std::cout << "Opened character device: " << device << '\n';
        demo::Stream stream(fd.get());
        const std::vector<std::string> messages = self_test
            ? std::vector<std::string>{"Hello from C++ guest!", "こんにちは、ホスト！", "", std::string(demo::max_frame, 'x')}
            : std::vector<std::string>{message};
        for (const auto& request : messages) {
            stream.send(request);
            std::cout << "guest -> host (" << request.size() << " bytes): " << request.substr(0, 80) << '\n';
            std::string reply;
            if (!stream.receive(reply)) throw std::runtime_error("host disconnected");
            if (reply != request) throw std::runtime_error("echo mismatch");
            std::cout << "host -> guest (" << reply.size() << " bytes): " << reply.substr(0, 80) << '\n';
        }
        std::cout << (self_test ? "VIRTIO_SELF_TEST_PASS" : "ECHO_OK") << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "guest error: " << error.what() << '\n';
        return 1;
    }
}
