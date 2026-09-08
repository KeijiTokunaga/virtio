#include "transport.hpp"
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc != 2 || std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: host /absolute/path/to/qemu.sock\n";
        return argc == 2 ? 0 : 2;
    }
    try {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(argv[1]) >= sizeof(address.sun_path))
            throw std::invalid_argument("Unix socket path is too long");
        std::strcpy(address.sun_path, argv[1]);
        demo::Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        // QEMU listens; the host service is the socket client.
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            demo::system_error("connect (start QEMU first)");
        demo::nonblocking(fd.get());
        demo::Stream stream(fd.get());
        std::cout << "Connected to QEMU; waiting for the guest." << std::endl;
        std::string message;
        while (stream.receive(message, -1)) {
            // Hex preview keeps guest-controlled terminal escape sequences inert.
            std::cout << "received " << message.size() << " bytes; hex:";
            for (unsigned char ch : message.substr(0, 32))
                std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch);
            std::cout << std::dec << std::endl;
            stream.send(message);
        }
        std::cout << "QEMU disconnected." << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "host error: " << error.what() << '\n';
        return 1;
    }
}
