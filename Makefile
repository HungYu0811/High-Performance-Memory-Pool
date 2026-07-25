CXX      = g++
CXXFLAGS = -std=c++17 -pthread -O2 -Wall -Wextra
DBGFLAGS = -DPOOL_DEBUG_LOG

# default `make` = clean then Release build (no logging, -O2 on)
# `all` depends on `clean` so stale binaries are always removed before compiling
all: clean main benchmark

main: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o main

benchmark: benchmark.cpp
	$(CXX) $(CXXFLAGS) benchmark.cpp -o benchmark

# `make debug` = clean then rebuild with POOL_DEBUG_LOG
debug: CXXFLAGS += $(DBGFLAGS)
debug: all

clean:
	rm -f main benchmark

.PHONY: all debug clean
