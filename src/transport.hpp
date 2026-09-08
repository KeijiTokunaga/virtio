#pragma once

// ゲストのデバイスfdとホストのソケットfdに共通の、アプリ用通信処理。
// ここでの「transport」は改行区切りの送受信を指す。
// virtioのPCI/MMIO transportやvirtqueueを実装しているわけではない。

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
// この上限はサンプル独自のプロトコルであり、virtioの転送サイズ上限ではない。
// 終端の改行は含めない。空の本文も有効で、その場合は改行だけを送る。
constexpr std::size_t max_frame = 4096;

// システムコール失敗直後のerrnoを、操作名付きのC++例外へ変換する。
[[noreturn]] inline void system_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// RAII: fdの寿命をC++オブジェクトの寿命に合わせ、例外時も必ずcloseする。
// コピーを禁止し、同じfdを二重に閉じないよう所有者を1つに限定する。
// get()は所有権を渡さない。Fdはそれを借りるStreamより長く生存させる。
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
    // 既存のフラグを維持したままO_NONBLOCKだけを追加する。
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        system_error("fcntl");
}

// 通常のI/Oエラーと期限切れを区別できるようにした例外型。
class Timeout : public std::runtime_error {
public:
    Timeout() : std::runtime_error("communication timed out") {}
};

// デバイスもUnixソケットもバイトストリームであり、メッセージ境界を保存しない。
// 例えばsend("abc")でも、readは"a"と"bc\n"の2回に分かれる可能性がある。
// 逆に"abc\ndef\n"を1回で読むこともあるため、未処理分をpending_に保持する。
// 呼び出し側はfdをO_NONBLOCKに設定すること。Streamは単一スレッドで利用する。
class Stream {
    // 実時刻の補正に影響されない時計で、経過時間を測る。
    using Clock = std::chrono::steady_clock;
    struct Deadline {
        // 1回のsend/receive全体で同じ期限を使う。
        // 部分転送やEINTRのたびに期限を延ばすと、いつまでも終了しなくなる。
        explicit Deadline(int timeout_ms)
            : infinite(timeout_ms < 0), end(Clock::now() + std::chrono::milliseconds(timeout_ms)) {}
        bool infinite;
        Clock::time_point end;
    };
public:
    explicit Stream(int fd) : fd_(fd) {}

    // 本文全体と区切りの改行を送り切る。成功は相手のアプリによる処理完了を意味しない。
    void send(std::string_view payload, int timeout_ms = 5000) {
        // 区切り文字が本文にあると1要求が複数フレームになるので、送信前に拒否する。
        if (payload.size() > max_frame || payload.find('\n') != std::string_view::npos)
            throw std::invalid_argument("frame must be <= 4096 bytes and contain no newline");
        std::string frame(payload);
        frame += '\n';
        Deadline deadline(timeout_ms);
        std::size_t offset = 0;
        while (offset < frame.size()) {
            // POLLOUTは「書き込める可能性がある」という通知。
            // 全バイトを書ける保証はないため、実際に書けた分だけoffsetを進める。
            wait(POLLOUT, deadline);
            const auto count = ::write(fd_, frame.data() + offset, frame.size() - offset);
            if (count < 0) {
                // EINTR: シグナルで中断。EAGAIN/EWOULDBLOCK: 今は書けない。
                // いずれもデータを進めず、元の期限で待ち直す。
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                system_error("write");
            }
            if (count == 0) throw std::runtime_error("zero-length write");
            offset += static_cast<std::size_t>(count);
        }
    }

    // 完全な1フレームをpayloadへ返す。本文が空でもtrueを返す。
    // falseはフレーム途中ではないEOF（相手の切断）。途中切断は例外にする。
    // 負のtimeout_msは無期限待ち。期限切れでもpending_は次回の呼び出しまで保持する。
    bool receive(std::string& payload, int timeout_ms = 5000) {
        Deadline deadline(timeout_ms);
        while (true) {
            // OSから読む前に、前回まとめて受信したデータを確認する。
            const auto newline = pending_.find('\n');
            if (newline != std::string::npos) {
                if (newline > max_frame) throw std::runtime_error("frame too large");
                payload = pending_.substr(0, newline);
                // 次のフレームのデータは削除せず、次回receiveで使用する。
                pending_.erase(0, newline + 1);
                return true;
            }
            // 改行が届かない場合も上限を確認し、受信バッファーの増大を防ぐ。
            if (pending_.size() > max_frame) throw std::runtime_error("frame too large");
            wait(POLLIN, deadline);
            char buffer[4096];
            const auto count = ::read(fd_, buffer, sizeof(buffer));
            if (count < 0) {
                // poll後でも状態が変わり得るため、一時的な読み取り不可は再試行する。
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                system_error("read");
            }
            if (count == 0) {
                // 非ブロッキングreadの「今はデータがない」はEAGAIN。
                // 0バイトはEOFとして扱い、区切りまで届かなかった本文を成功にしない。
                if (!pending_.empty()) throw std::runtime_error("peer disconnected mid-frame");
                return false;
            }
            pending_.append(buffer, static_cast<std::size_t>(count));
        }
    }

private:
    // fdの準備ができるか期限が切れるまで待機する。待機中にCPUを回し続けない。
    void wait(short events, const Deadline& deadline) const {
        while (true) {
            int timeout = -1;
            if (!deadline.infinite) {
                const auto remaining = deadline.end - Clock::now();
                if (remaining <= Clock::duration::zero()) throw Timeout();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                // pollは整数ミリ秒を受け取る。1ms未満を0へ切り捨てて即時終了せず、
                // 少なくとも1ms待つ。大きな値はintの範囲に収める。
                timeout = static_cast<int>(ms >= INT_MAX ? INT_MAX : ms + 1);
            }
            // eventsが待ちたい状態、reventsがOSから返された状態。
            pollfd pfd{fd_, events, 0};
            const int result = ::poll(&pfd, 1, timeout);
            if (result < 0) {
                if (errno == EINTR) continue;
                system_error("poll");
            }
            if (result == 0) throw Timeout();
            if (pfd.revents & POLLNVAL) throw std::runtime_error("invalid descriptor");
            // HUP（切断）でも受信済みデータが残る場合があるので、ここでは捨てない。
            // HUP/ERRを含め呼び出し元に戻し、read/writeでデータ・EOF・エラーを判定する。
            if (pfd.revents & (events | POLLHUP | POLLERR)) return;
        }
    }

    int fd_;               // 借用しているfd。closeするのは所有者のFd。
    std::string pending_;  // まだ返していない受信データ。複数フレームを含む場合もある。
};
} // namespace demo
