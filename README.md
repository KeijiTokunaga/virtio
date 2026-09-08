# C++で学ぶvirtio：ゲストのデバイスファイルからホストへ

Linuxゲストが `/dev/virtio-ports/org.example.echo` を `open / write / read` し、ホストのC++プログラムが同じ内容を返すサンプルです。**ゲストとホストの通信プログラムはC++17**。Linux標準の **virtio-serial（virtio-consoleのマルチポート機能）** を使います。

このMac（Apple Silicon）で、QEMU + **Hypervisor.framework / HVF** による実通信を検証済みです。CPUエミュレーションのTCGでも動作確認しています。独自カーネルドライバーの実装は含みません。

## まず動かす

この作業環境にはQEMUをインストール済みです。リポジトリのディレクトリで実行します。

```bash
make test
make demo DEMO_ARGS=--accel=hvf
```

`make demo` は小さなARM64 Linuxをメモリ上で起動し、デバイスファイルの確認、C++クライアントの実行、往復通信、シャットダウンまで自動で行います。ディスクイメージ、OSの手動インストール、ゲストのネットワーク接続は不要です。

別の環境で実行する場合の前提:

- macOSまたはLinux、C++17コンパイラ、Make、Python 3.12以降、curl、QEMU。
- macOSでは `brew install qemu`、Debian/Ubuntuでは `sudo apt install build-essential qemu-system-arm curl python3` が導入例です。
- Pythonはダウンロード・initramfs作成・プロセス起動用です。virtioのデータ送受信はC++が行います。
- 初回はZig 0.15.2（Linux用クロスコンパイラ）、AlpineのLinuxカーネルとBusyBoxを公式HTTPS配布元から取得します。合計約90〜100MB。SHA-256を確認し、`.cache/` に保存します。
- 固定したAlpineパッケージが配布元から削除された場合は、URL・ハッシュ・カーネル設定ファイル名を合わせて更新する必要があります。

CPU実行方式は切り替えられます。自動デモのゲストはどの場合もARM64です。

```bash
# ARM64 macOS: 実際のハイパーバイザーを利用
make demo DEMO_ARGS=--accel=hvf

# ARM64 / x86_64のmacOS・Linux: CPUエミュレーション（デフォルト）
make demo

# ARM64 Linux: KVM対応環境向け（このMacでは未検証）
make demo DEMO_ARGS=--accel=kvm
```

成功時の出力（抜粋）:

```text
/dev/virtio-ports/org.example.echo -> /dev/vport0p1
crw------- ... /dev/vport0p1
/sys/bus/virtio/drivers/virtio_console
Opened character device: /dev/virtio-ports/org.example.echo
guest -> host (30 bytes): こんにちは、ホスト！
host -> guest (30 bytes): こんにちは、ホスト！
VIRTIO_SELF_TEST_PASS
ECHO_OK
DEMO_PASS
PASS: 5 real virtio round trips, including a second device open.
```

完全なログは `build/logs/guest.log` と `build/logs/host.log` に残ります。次回実行時に上書きします。失敗時にもログを残し、起動したプロセスを終了します。

## 通信経路

```text
Linuxゲスト                                    ホスト
src/guest.cpp
  open / write / read
         |
/dev/virtio-ports/org.example.echo
  -> /dev/vport0p1（キャラクターデバイス）
         |
Linux virtio_console ドライバー
         |
送信・受信 virtqueue ← ゲストメモリ → QEMU virtio-serialデバイス
                                               |
                                         QEMU chardev
                                               |
                                         Unix domain socket
                                               |
                                         src/host.cpp
                                           echo返信
```

HVF/KVMは仮想CPUの実行を担当し、QEMUはvirtioデバイスを提供します。TCGに切り替えても、このI/O経路は同じです。ホストのC++プログラムはQEMUが公開するソケットへ接続し、QEMUがvirtqueueとソケットの間を橋渡しします。

**ゲスト側はソケットを使いません。** `guest.cpp` の中心は以下です。

```cpp
demo::Fd fd(::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
demo::Stream stream(fd.get());
stream.send(request);       // 内部で poll() と write()
stream.receive(reply);      // 内部で poll() と read()
```

`fstat()` と `S_ISCHR()` でキャラクターデバイスであることを確認します。ただし、それだけではvirtioだと断定できません。デモではsysfsのポート名とドライバーも表示し、`virtio_console` に接続されたデバイスであることを確認します。

## コードを読む順番

| ファイル | 役割 |
| --- | --- |
| `src/guest.cpp` | デバイスを開き、要求を書いて返信を読む |
| `src/host.cpp` | QEMUのUnixソケットに接続し、受信内容を返す |
| `src/transport.hpp` | fdのRAII管理、改行フレーム、部分読み書き、期限付きpoll |
| `qemu-virtio.sh` | 既存VMのQEMU起動コマンドへvirtioポートを追加 |
| `scripts/guest-init.sh` | 最小LinuxのPID 1。デバイス確認、テスト、電源断 |
| `scripts/prepare_demo.py` | クロスコンパイル、カーネルとinitramfsの準備 |
| `scripts/run_demo.py` | QEMUとC++ホストの起動、結果判定、後片付け |
| `tests/test_transport.cpp` | VM不要の通信処理テスト |

## virtqueueの中で起きること

1. QEMUがPCIバス上にvirtio-serialコントローラーを公開し、Linuxがvirtioドライバーを対応付けます。
2. ゲストドライバーとデバイスが機能を交渉し、virtqueueを用意します。マルチポートの制御キューでは、ポート名や接続・開閉状態などを通知します。
3. ゲストの `write()` を受けたドライバーが、ゲストメモリ上の送信バッファーを指すdescriptorを送信キューに登録します。利用可能なバッファーがあることをデバイスへ通知します。通知には抑制・省略の最適化があります。
4. QEMUがdescriptorとバッファーを処理し、chardevを介してホストソケットへバイト列を渡します。処理済みバッファーはused側でゲストへ返却します。
5. ホストの返信はQEMUに戻り、ゲストが受信キューへ事前登録したバッファーに格納されます。ドライバーが完了を処理すると、ゲストの `read()` で取得できます。

アプリケーションがvirtqueueやホストメモリを直接操作することはありません。このサンプルで利用できるホスト機能はechoだけです。バイト列にどのような意味を持たせるかは、アプリケーション側のプロトコルで定めます。

## QEMU引数の意味

デモは次の3つを組み合わせています。

```bash
-device virtio-serial-pci,id=samplevirtio
-chardev socket,id=samplechannel,path=/tmp/PRIVATE/echo.sock,server=on,wait=on
-device virtserialport,bus=samplevirtio.0,chardev=samplechannel,name=org.example.echo
```

| 指定 | 意味 |
| --- | --- |
| `virtio-serial-pci` | PCI接続のvirtio-serialコントローラー |
| `chardev socket` | ホスト側へ公開するUnixソケット |
| `server=on` | QEMUがlistenし、C++ホストがconnectする |
| `wait=on` | C++ホストが接続するまでVMの起動を待つ |
| `virtserialport` | コントローラー上のポートとchardevを対応付ける |
| `name=org.example.echo` | ゲスト側のポート名 |

通常のLinuxではudevが `/dev/virtio-ports/` の名前付きリンクを作ります。自動デモはudevを省いた最小initramfsなので、sysfsで名前を照合して同じリンクを作成します。リンク先の `/dev/vportXpY` はカーネル/devtmpfsが提供する本物のデバイスです。通常ファイルや疑似端末で代用していません。

固定したLinuxカーネルには `CONFIG_VIRTIO_CONSOLE=y` と `CONFIG_VIRTIO_PCI=y` が組み込まれており、デモではモジュール読み込みは不要です。

## 既存のLinux VMで手動実行する

自動デモ以外のディストリビューション・CPU構成でも、C++をそのLinux向けにビルドして利用できます。

### 1. ホスト側をビルドし、VMを起動

```bash
make
export VIRTIO_SOCKET="$(mktemp -d /tmp/virtio-echo.XXXXXX)/echo.sock"
echo "$VIRTIO_SOCKET"
```

既に使っているQEMU起動コマンドの先頭に `bash ./qemu-virtio.sh` を付けます。例えばBIOS起動可能なx86_64 Linuxのqcow2ディスクなら:

```bash
bash ./qemu-virtio.sh qemu-system-x86_64 \
  -machine q35 -accel tcg -m 2048 \
  -drive file=/absolute/path/linux.qcow2,format=qcow2,if=virtio \
  -snapshot
```

ディスクパスは実際の値に置き換えます。既存VMと同じディスクを同時に開かないでください。上の `-snapshot` の例ではディスク変更は終了時に破棄します。ARM VMなら、元の `qemu-system-aarch64 -machine virt ...` のCPU・UEFI・カーネル設定を維持してラップします。PCI対応マシンが必要です。

このラッパーでは `wait=off` を使うため、ホストサービスの接続前でもVMが起動します。

### 2. 別のホストターミナルでサービスを起動

```bash
./build/host /tmp/virtio-echo.XXXXXX/echo.sock
```

先ほど表示した実際のパスを指定します。`Connected to QEMU; waiting for the guest.` が出てからゲストプログラムを実行してください。

### 3. ゲストでC++をビルドし、通信

`src/guest.cpp` と `src/transport.hpp` をゲストの同じディレクトリにコピーします。既存のSCPや共有フォルダーなどを使えます。

```bash
# 以下はLinuxゲスト内
c++ -std=c++17 -O2 -Wall -Wextra guest.cpp -o guest
sudo modprobe virtio_console
ls -l /dev/virtio-ports/
sudo ./guest 'こんにちは、ホスト！'
sudo ./guest --self-test
```

ARM64 Linuxゲストなら、自動デモで作成した静的リンク済みの `build/guest-linux-aarch64` をコピーしてそのまま実行することもできます。**macOS上の `make` で生成する `build/guest` はmacOSバイナリ**なので、Linuxへコピーして使うことはできません。

udevの名前付きリンクがない場合:

```bash
cat /sys/class/virtio-ports/vport*/name
# org.example.echo と対応する実体を指定。番号は環境で変わる。
sudo ./guest --device /dev/vport0p1 'Hello'
```

`strace` が入っていれば、ゲストのシステムコールを観察できます。

```bash
sudo strace -e trace=openat,read,write,poll ./guest Hello
```

## プロトコルと制約

- UTF-8文字列 + 改行1バイト。本文に改行は含めません。フレーム処理自体はバイト列を扱い、ホストはUTF-8検証を行わずそのまま返信します。
- 本文の上限は4096バイト。日本語の文字数とは異なります。
- `write()` と `read()` の回数・サイズは対応しません。フレームの分割と結合を処理します。
- ゲストの送信・受信期限はそれぞれ5秒。ホストは受信を待ち続け、返信には5秒の期限があります。
- 自動再接続や要求IDによる再送処理は実装していません。タイムアウト後に古い返信が残る場合は、VMとホストサービスを再起動してください。
- 1ポートにつきゲストクライアントは1つです。同時実行しないでください。
- ttyではないため、baud rateやtermiosの設定は不要です。

## 検証結果

2026-09-08、Apple Silicon macOS上で確認:

| 項目 | 結果 |
| --- | --- |
| QEMU | Homebrewで11.1.1をインストール |
| Linuxゲスト | Alpine Linux由来の6.18.49-0-virt / aarch64 |
| C++通信処理の単体テスト | 7件成功 |
| QEMU + TCGの実通信 | 5往復成功 |
| QEMU + HVFの実通信 | 5往復成功 |
| ゲストデバイス | `/dev/vport0p1`、キャラクターデバイス、`virtio_console` |

単体テストは分割・結合フレーム、日本語・空文字・最大サイズ、不正な送信フレーム、過大な受信フレーム、正常切断、途中切断、書き込みが進まない場合のタイムアウトを扱います。実通信では英語、日本語、空文字、4096バイト、デバイスを開き直した別プロセスからの要求を検証します。

## 困ったとき

| 症状 | 確認すること |
| --- | --- |
| デバイスがない | QEMU追加引数、virtio_console対応、sysfsのポート名、udev |
| Permission denied | ゲストでsudoを使用。常用するなら対象ポートに絞ったudev権限を設定 |
| EOF / timeout / write error | ホストサービスが接続済みか、別プロセスがポートを使用していないか |
| ソケットへの接続失敗 | QEMUが起動済みか、ソケットパスが一致するか |
| Socket path already exists | 新しいmktempディレクトリを使う。ラッパーは既存ソケットを削除しない |
| HVF/KVMが使えない | 対応OS・CPU・実行権限を確認。CPUエミュレーションなら `make demo` |
| QEMUのbindがOperation not permitted | 実行環境のサンドボックスによるUnixソケット制限を確認 |

## 一次資料

- [QEMUのchardev引数](https://www.qemu.org/docs/master/system/qemu-manpage.html)
- [Linux: Virtio on Linux](https://www.kernel.org/doc/html/v6.8/driver-api/virtio/virtio.html)
- [QEMU virtio-console実装](https://github.com/qemu/qemu/blob/master/hw/char/virtio-console.c)
- [Linux virtio_console.c](https://github.com/torvalds/linux/blob/master/drivers/char/virtio_console.c)
- [Alpine Linuxパッケージ配布元](https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/)
- [Zig公式ダウンロードとチェックサム](https://ziglang.org/download/)

次の学習段階は、Linuxのファイル操作からvirtqueueへの登録箇所と、QEMU側の受信処理を追うことです。独自virtioデバイスへ進む場合は、その後にdevice ID、feature交渉、キュー構成、ゲストドライバー、QEMU側デバイス実装を設計します。
