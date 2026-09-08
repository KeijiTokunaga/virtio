// Linuxゲストで動く、virtioポートの利用例。
// このアプリはFEそのものではなく、デバイスファイルを通じてFEを利用する。
//
// guest.cpp → /dev/vportXpY → Linux virtio_console（FE）
//           → virtqueue → QEMUのvirtio-serial（BE）→ host.cpp
//
// virtqueueの初期化・descriptorの登録・割り込み処理はLinuxドライバーが担当する。
// アプリからは通常のファイルと同じようにopen/read/writeを呼び出せる。
#include "transport.hpp"
#include <csignal>
#include <iostream>
#include <sys/stat.h>
#include <vector>

int main(int argc, char** argv) {
    // 相手の切断時にプロセスが突然終了するのを避け、writeのエラーとして扱う。
    std::signal(SIGPIPE, SIG_IGN);
    try {
        // QEMUのname=と一致する名前付きリンク。vportの番号は環境で変わるため、
        // 通常はこちらを使う。リンクがない環境では--deviceで実体を指定できる。
        std::string device = "/dev/virtio-ports/org.example.echo";
        std::string message = "Hello from C++ guest!";
        bool self_test = false;
        bool have_message = false;
        // 通常は1メッセージだけ送信する。--self-testは境界値を含む4種類を送る。
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

        // O_RDWR: 同じポートで送信と受信の両方を行う。
        // O_NONBLOCK: read/writeで無期限に止まらず、Stream内のpollで期限を管理する。
        // O_CLOEXEC: 将来execを呼ぶ場合も、このfdを別プログラムへ引き継がない。
        // ここで開くのはLinuxのキャラクターデバイスであり、ソケットではない。
        demo::Fd fd(::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
        // 通常ファイルを誤って指定していないか確認する。
        // S_ISCHRだけではvirtioとは断定できないため、デモではsysfsのdriverも表示する。
        struct stat info{};
        if (::fstat(fd.get(), &info) < 0) demo::system_error("fstat");
        if (!S_ISCHR(info.st_mode)) throw std::runtime_error("expected a character device");
        std::cout << "Opened character device: " << device << '\n';
        // Streamはfdを借用する。スコープ終了時のcloseはFdが担当する。
        demo::Stream stream(fd.get());
        // 長さの単位は文字数ではなくバイト数。日本語はUTF-8のまま送受信する。
        const std::vector<std::string> messages = self_test
            ? std::vector<std::string>{"Hello from C++ guest!", "こんにちは、ホスト！", "", std::string(demo::max_frame, 'x')}
            : std::vector<std::string>{message};
        // 要求と返信を1組ずつ処理する。要求IDを持たないため同時に複数要求を送らない。
        for (const auto& request : messages) {
            // writeの成功は送信データをドライバーへ渡せたという意味で、
            // ホストアプリの処理完了ではない。下の返信照合で往復の完了を確認する。
            stream.send(request);
            std::cout << "guest -> host (" << request.size() << " bytes): " << request.substr(0, 80) << '\n';
            std::string reply;
            if (!stream.receive(reply)) throw std::runtime_error("host disconnected");
            // 表示は先頭80バイトに省略しても、照合には返信全体を使う。
            if (reply != request) throw std::runtime_error("echo mismatch");
            std::cout << "host -> guest (" << reply.size() << " bytes): " << reply.substr(0, 80) << '\n';
        }
        // 全要求の照合が成功した場合だけ成功マーカーを出す。
        std::cout << (self_test ? "VIRTIO_SELF_TEST_PASS" : "ECHO_OK") << std::endl;
    } catch (const std::exception& error) {
        // 例外で抜けてもFdのデストラクターがデバイスを閉じる。
        // 非0の終了コードをゲストのinitスクリプトが検出する。
        std::cerr << "guest error: " << error.what() << '\n';
        return 1;
    }
}
