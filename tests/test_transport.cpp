#include "transport.hpp"
#include <csignal>
#include <functional>
#include <iostream>
#include <sys/socket.h>

namespace {
void check(bool condition) {
    if (!condition) throw std::runtime_error("check failed");
}
template <typename Error, typename Action> void expect(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("expected exception was not thrown");
}
struct Pair {
    static int* create(int (&fds)[2]) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) demo::system_error("socketpair");
        return fds;
    }
    int fds[2]{};
    demo::Fd a{create(fds)[0]}, b{fds[1]};
    demo::Stream first{a.get()}, second{b.get()};
    Pair() { demo::nonblocking(a.get()); demo::nonblocking(b.get()); }
    void raw(std::string_view data) {
        check(::write(b.get(), data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    }
};
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"fragmented and coalesced frames", [] {
            Pair p;
            std::string message;
            p.raw("par");
            expect<demo::Timeout>([&] { p.first.receive(message, 10); });
            p.raw("tial\nnext\n");
            check(p.first.receive(message) && message == "partial");
            check(p.first.receive(message) && message == "next");
        }},
        {"empty, UTF-8, max frame round trips", [] {
            Pair p;
            for (const std::string& payload : {std::string{}, std::string("こんにちは"), std::string(demo::max_frame, 'x')}) {
                p.first.send(payload);
                std::string received;
                check(p.second.receive(received) && received == payload);
                p.second.send(received);
                check(p.first.receive(received) && received == payload);
            }
        }},
        {"invalid outgoing frames", [] {
            Pair p;
            expect<std::invalid_argument>([&] { p.first.send("a\nb"); });
            expect<std::invalid_argument>([&] { p.first.send(std::string(demo::max_frame + 1, 'x')); });
        }},
        {"oversize incoming frame", [] {
            Pair p;
            p.raw(std::string(demo::max_frame + 1, 'x') + "\n");
            std::string message;
            expect<std::runtime_error>([&] { p.first.receive(message); });
        }},
        {"clean EOF", [] {
            Pair p;
            ::shutdown(p.b.get(), SHUT_RDWR);
            std::string message;
            check(!p.first.receive(message));
        }},
        {"truncated EOF", [] {
            Pair p;
            p.raw("incomplete");
            ::shutdown(p.b.get(), SHUT_WR);
            std::string message;
            expect<std::runtime_error>([&] { p.first.receive(message); });
        }},
        {"write backpressure deadline", [] {
            Pair p;
            const std::string block(4096, 'x');
            while (::write(p.a.get(), block.data(), block.size()) > 0) {}
            check(errno == EAGAIN || errno == EWOULDBLOCK);
            expect<demo::Timeout>([&] { p.first.send("blocked", 10); });
        }},
    };
    try {
        for (const auto& test : tests) {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
