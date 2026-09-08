// virtioを起動せず、Streamのフレーム処理と異常系を検証する単体テスト。
// socketpairはストリームの分割・結合を試すための道具であり、virtqueueの模擬実装ではない。
// FE/BEを含む実通信の検証はmake demoで行う。
#include "transport.hpp"
#include <csignal>
#include <functional>
#include <iostream>
#include <sys/socket.h>

namespace {
void check(bool condition) {
    if (!condition) throw std::runtime_error("check failed");
}
// 指定型の例外が発生すれば成功。それ以外の例外は上位へ伝え、テスト失敗にする。
template <typename Error, typename Action> void expect(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("expected exception was not thrown");
}
// 接続済みの2つのソケットを用意し、両端を本番と同じノンブロッキングにする。
struct Pair {
    static int* create(int (&fds)[2]) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) demo::system_error("socketpair");
        return fds;
    }
    int fds[2]{};
    demo::Fd a{create(fds)[0]}, b{fds[1]};
    demo::Stream first{a.get()}, second{b.get()};
    Pair() { demo::nonblocking(a.get()); demo::nonblocking(b.get()); }
    // Stream::sendの検査を通さず、途中フレームや不正な長さを受信側へ注入する。
    // このヘルパーは少量データが一度で書けたことも検査し、書けなければ失敗させる。
    void raw(std::string_view data) {
        check(::write(b.get(), data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    }
};
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"fragmented and coalesced frames", [] {
            // 改行なしでは完了しない。期限切れ後も保持した"par"と続きが結合され、
            // まとめて届いた2番目のフレームも次のreceiveで取得できることを確認する。
            Pair p;
            std::string message;
            p.raw("par");
            expect<demo::Timeout>([&] { p.first.receive(message, 10); });
            p.raw("tial\nnext\n");
            check(p.first.receive(message) && message == "partial");
            check(p.first.receive(message) && message == "next");
        }},
        {"empty, UTF-8, max frame round trips", [] {
            // 0バイト本文とEOFを区別し、日本語や上限サイズも両方向で欠損なく返す。
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
            // 相手へ送る前に、区切り文字の混入とサイズ超過を検出する。
            Pair p;
            expect<std::invalid_argument>([&] { p.first.send("a\nb"); });
            expect<std::invalid_argument>([&] { p.first.send(std::string(demo::max_frame + 1, 'x')); });
        }},
        {"oversize incoming frame", [] {
            // 相手側がプロトコルを守らなくても、受信側自身で上限を検査する。
            Pair p;
            p.raw(std::string(demo::max_frame + 1, 'x') + "\n");
            std::string message;
            expect<std::runtime_error>([&] { p.first.receive(message); });
        }},
        {"clean EOF", [] {
            // 未完の本文がない切断は例外ではなくfalseを返す。
            Pair p;
            ::shutdown(p.b.get(), SHUT_RDWR);
            std::string message;
            check(!p.first.receive(message));
        }},
        {"truncated EOF", [] {
            // 改行が来る前の切断で、不完全な本文を成功として返してはいけない。
            Pair p;
            p.raw("incomplete");
            ::shutdown(p.b.get(), SHUT_WR);
            std::string message;
            expect<std::runtime_error>([&] { p.first.receive(message); });
        }},
        {"write backpressure deadline", [] {
            // 相手が読まない状況を作り、送信バッファーを満たしてEAGAINにする。
            // 書き込み可能になるまで永久に待たず、指定期限で終了することを確認する。
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
