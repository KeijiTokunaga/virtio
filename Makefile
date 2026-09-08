# 通常ビルドは実行中のホストOS向け。macOSで作ったbuild/guestはLinuxでは動かない。
# 自動デモ用Linuxバイナリはprepare_demo.pyが別途クロスコンパイルする。
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

.PHONY: all test demo clean
all: build/host build/guest

build:
	mkdir -p build

build/host: src/host.cpp src/transport.hpp | build
	$(CXX) $(CXXFLAGS) -Isrc src/host.cpp -o $@

build/guest: src/guest.cpp src/transport.hpp | build
	$(CXX) $(CXXFLAGS) -Isrc src/guest.cpp -o $@

build/test-transport: tests/test_transport.cpp src/transport.hpp | build
	$(CXX) $(CXXFLAGS) -Isrc -pthread tests/test_transport.cpp -o $@

# VMを使わず、socketpairで共通のフレーム処理・エラー処理を検証する。
test: all build/test-transport
	./build/test-transport

# 例: make demo DEMO_ARGS=--accel=hvf（Apple Siliconのハイパーバイザーを利用）
demo: all
	python3 scripts/run_demo.py $(DEMO_ARGS)

# ダウンロードキャッシュは残し、生成したバイナリ・起動ファイル・ログだけ削除する。
clean:
	rm -rf build
