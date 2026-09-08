// ホストOSで動くechoサービス。QEMUが公開するUnixソケットに接続する。
// virtioのBEはQEMU内にあり、このC++プログラムはBEの先でデータを処理する。
// ゲストメモリやvirtqueueには直接触れず、通常のバイトストリームを読み書きする。
#include "transport.hpp"
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char** argv) {
    // 切断後のwriteでSIGPIPEにより終了せず、例外処理でエラーを報告する。
    std::signal(SIGPIPE, SIG_IGN);
    if (argc != 2 || std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: host /absolute/path/to/qemu.sock\n";
        return argc == 2 ? 0 : 2;
    }
    try {
        // AF_UNIXはホスト内のプロセス間通信。TCP/IPやゲストのNICは使用しない。
        // sun_pathは固定長配列なので、終端のNULを含めて収まるか先に確認する。
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(argv[1]) >= sizeof(address.sun_path))
            throw std::invalid_argument("Unix socket path is too long");
        std::strcpy(address.sun_path, argv[1]);
        demo::Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
        // 「echoサービス」であっても、ソケット上では接続する側（client）。
        // QEMUの-chardev ...,server=onがlistenする側になる。
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            demo::system_error("connect (start QEMU first)");
        // 接続後のread/writeはノンブロッキングにして、Streamで待ち時間を管理する。
        demo::nonblocking(fd.get());
        demo::Stream stream(fd.get());
        std::cout << "Connected to QEMU; waiting for the guest." << std::endl;
        std::string message;
        // -1は受信の無期限待ち。ゲストから要求がない間はpollで待機する。
        // QEMUがソケットを閉じ、途中のフレームも残っていなければループを終了する。
        while (stream.receive(message, -1)) {
            // 先頭32バイトを16進で表示し、ゲスト由来の端末制御文字の実行を避ける。
            std::cout << "received " << message.size() << " bytes; hex:";
            for (unsigned char ch : message.substr(0, 32))
                std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch);
            std::cout << std::dec << std::endl;
            // 返信は省略せず全バイトを返す。改行区切りの付与はStreamが担当する。
            // 送信は既定の5秒で打ち切り、相手が読まない場合の無期限停止を避ける。
            stream.send(message);
        }
        std::cout << "QEMU disconnected." << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "host error: " << error.what() << '\n';
        return 1;
    }
}
