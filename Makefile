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

test: all build/test-transport
	./build/test-transport

demo: all
	python3 scripts/run_demo.py $(DEMO_ARGS)

clean:
	rm -rf build
