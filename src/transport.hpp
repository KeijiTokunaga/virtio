#pragma once

#include <cerrno>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace demo {
constexpr std::size_t max_frame = 4096;

[[noreturn]] inline void system_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// A descriptor has exactly one owner. Stream only borrows it.
class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {
        if (fd < 0) system_error("open/socket");
    }
    ~Fd() { ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

inline void nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        system_error("fcntl");
}

class Timeout : public std::runtime_error {
public:
    Timeout() : std::runtime_error("communication timed out") {}
};

// A stream carries bytes, not messages. Newline delimits a frame.
// read() can split one frame or return several frames at once.
class Stream {
    using Clock = std::chrono::steady_clock;
    struct Deadline {
        explicit Deadline(int timeout_ms)
            : infinite(timeout_ms < 0), end(Clock::now() + std::chrono::milliseconds(timeout_ms)) {}
        bool infinite;
        Clock::time_point end;
    };
public:
    explicit Stream(int fd) : fd_(fd) {}

    void send(std::string_view payload, int timeout_ms = 5000) {
        if (payload.size() > max_frame || payload.find('\n') != std::string_view::npos)
            throw std::invalid_argument("frame must be <= 4096 bytes and contain no newline");
        std::string frame(payload);
        frame += '\n';
        Deadline deadline(timeout_ms);
        std::size_t offset = 0;
        while (offset < frame.size()) {
            wait(POLLOUT, deadline);
            const auto count = ::write(fd_, frame.data() + offset, frame.size() - offset);
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                system_error("write");
            }
            if (count == 0) throw std::runtime_error("zero-length write");
            offset += static_cast<std::size_t>(count);
        }
    }

    // false means a clean EOF between frames; truncated frames are errors.
    // A negative timeout permits the host service to wait indefinitely.
    bool receive(std::string& payload, int timeout_ms = 5000) {
        Deadline deadline(timeout_ms);
        while (true) {
            const auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                if (newline > max_frame) throw std::runtime_error("frame too large");
                payload = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                return true;
            }
            if (pending_.size() > max_frame) throw std::runtime_error("frame too large");
            wait(POLLIN, deadline);
            char buffer[4096];
            const auto count = ::read(fd_, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                system_error("read");
            }
            if (count == 0) {
                if (!pending_.empty()) throw std::runtime_error("peer disconnected mid-frame");
                return false;
            }
            pending_.append(buffer, static_cast<std::size_t>(count));
        }
    }

private:
    void wait(short events, const Deadline& deadline) const {
        while (true) {
            int timeout = -1;
            if (!deadline.infinite) {
                const auto remaining = deadline.end - Clock::now();
                if (remaining <= Clock::duration::zero()) throw Timeout();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                timeout = static_cast<int>(ms >= INT_MAX ? INT_MAX : ms + 1);
            }
            pollfd pfd{fd_, events, 0};
            const int result = ::poll(&pfd, 1, timeout);
            if (result < 0) {
                if (errno == EINTR) continue;
                system_error("poll");
            }
            if (result == 0) throw Timeout();
            if (pfd.revents & POLLNVAL) throw std::runtime_error("invalid descriptor");
            // On HUP, read remaining bytes first. read/write reports EOF/error.
            if (pfd.revents & (events | POLLHUP | POLLERR)) return;
        }
    }

    int fd_;
    std::string pending_;
};
} // namespace demo
