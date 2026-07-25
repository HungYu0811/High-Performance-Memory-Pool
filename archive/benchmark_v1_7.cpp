// benchmark_v1_7.cpp: v1.7-era pool baseline (bounds-checked deallocate from v1.6, but no
// in_use/double-free-detection -- that's a v1.8 addition), measured against the system
// allocator under single- and multi-threaded load. Deliberately exceeds a single 64KB chunk
// per thread to exercise the v1.6 dynamic-growth path.
//
// This file exists to complete a same-machine v1.7 -> v1.8 -> v1.9 interleaved-benchmark
// chain (see benchmark_v1_8.cpp / benchmark.cpp for the other two links). The ONLY intentional
// difference between the three files is the pool's in_use handling:
//   v1.7: no in_use tracking at all (this file)
//   v1.8: std::vector<bool> in_use (benchmark_v1_8.cpp)
//   v1.9: std::vector<uint8_t> in_use (benchmark.cpp)
// Everything else -- including the benchmark-methodology fixes below -- is kept IDENTICAL
// across all three files on purpose, so an interleaved comparison isolates the pool-code
// variable cleanly instead of also mixing in unrelated fixes as a second variable.
//
// Both fixes were discovered while cross-testing the v1.9 file on Windows (MinGW-w64 GCC
// 16.1.0) vs Linux (GCC 8.5.0), then backported here for consistency:
//
// 1. `volatile long long sink` added to bench_single/worker_pool/worker_sys. On the newer
//    compiler, the `system` (new/delete) single-threaded loop was silently optimized away
//    entirely, producing a nonsensical "inf Mops/sec" result: `Player{ ..., "bench" }` never
//    escapes the loop body and is never read before being destroyed, and "bench" (5 chars)
//    fits inside std::string's small-string-optimization buffer so construction never touches
//    the heap either -- the whole allocate/construct/destroy/free sequence has no observable
//    side effect, so the C++ standard permits a compiler to elide it entirely (allocation
//    elision). `sink += p->id` after each construction is a volatile write, a defined
//    observable side effect under the standard, so no compiler at any optimization level may
//    eliminate the loop.
//
// 2. `sink` is `long long`, not `long` -- `long` is 32-bit on Windows (LLP64) but 64-bit on
//    Linux (LP64); accumulating tens of millions of iterations into a 32-bit sink silently
//    overflowed (undefined behavior) on Windows. `long long` is guaranteed >=64-bit on both.
//
// Build:  cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
// Run:    ./benchmark            (or benchmark.exe on Windows)
//
// Output is a plain table of operations/sec. No logging (POOL_DEBUG_LOG off in Release).

#include <iostream>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <new>
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// ---- CentralArena (identical to main.cpp, kept self-contained for the benchmark) ----
struct Node { Node* next; };

class CentralArena {
private:
    char* baseAddress;
    size_t totalSize;
    std::atomic<size_t> offset;
    Node* freeCentralHead = nullptr;
    std::mutex arena_mtx;
public:
    CentralArena(size_t size) : totalSize(size), offset(0) {
#ifdef _WIN32
        baseAddress = (char*)VirtualAlloc(NULL, totalSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        baseAddress = (char*)mmap(NULL, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (!baseAddress) std::cerr << "[CRITICAL] arena alloc failed\n";
    }
    char* requestChunk(size_t chunkSize) {
        std::lock_guard<std::mutex> lock(arena_mtx);
        if (freeCentralHead != nullptr) {
            char* c = (char*)freeCentralHead;
            freeCentralHead = freeCentralHead->next;
            return c;
        }
        if (offset.load() + chunkSize > totalSize) return nullptr;
        char* a = baseAddress + offset.load();
        offset += chunkSize;
        return a;
    }
    void returnChunk(char* chunk) {
        std::lock_guard<std::mutex> lock(arena_mtx);
        Node* r = (Node*)chunk;
        r->next = freeCentralHead;
        freeCentralHead = r;
    }
    ~CentralArena() {
#ifdef _WIN32
        VirtualFree(baseAddress, 0, MEM_RELEASE);
#else
        munmap(baseAddress, totalSize);
#endif
    }
};
static CentralArena g_centralArena(400 * 1024 * 1024);

// v1.7-era pool: bounds-checked deallocate() (membership + alignment + tail-padding, added in
// v1.6) is present, but NOT the in_use/double-free-detection machinery (that's a v1.8 addition).
// This is the v1.7 baseline for the same-machine v1.7 -> v1.8 -> v1.9 chain: the ONLY
// intentional difference between this file and benchmark_v1_8.cpp is the presence/absence of
// in_use tracking. The benchmark-methodology fixes (volatile sink against dead-code elimination,
// long -> long long against 32-bit-long overflow on Windows) are kept IDENTICAL across all three
// files on purpose, so an interleaved comparison isolates the pool-code variable cleanly.
template <typename T>
class SimpleMemoryPool {
private:
    std::vector<char*> memory_list;
    Node* freeListHead = nullptr;
    size_t myChunkSize;
    size_t blockSize;
    void grow_pool() {
        Node* temp = freeListHead;
        char* newChunk = g_centralArena.requestChunk(myChunkSize);
        if (!newChunk) throw std::bad_alloc();
        memory_list.push_back(newChunk);

        blockSize = (sizeof(T) + 63) & ~63;
        if (blockSize > myChunkSize - 64) throw std::bad_alloc();

        size_t maxSlots = (myChunkSize - 64) / blockSize; // v1.7: local, not a member -- in_use
                                                           // doesn't exist yet, so nothing else
                                                           // needs this value outside grow_pool().

        freeListHead = (Node*)memory_list.back();
        Node* cur = freeListHead;
        for (size_t i = 0; i < maxSlots - 1; ++i) {
            cur->next = (Node*)((char*)cur + blockSize);
            cur = cur->next;
        }
        cur->next = temp;
    }
public:
    SimpleMemoryPool() {
        myChunkSize = 64 * 1024;
        grow_pool();
    }
    ~SimpleMemoryPool() {
        for (auto c : memory_list) g_centralArena.returnChunk(c);
    }
    void* allocate() {
        if (!freeListHead) grow_pool();
        Node* p = freeListHead;
        freeListHead = freeListHead->next;
        // v1.7: no in_use bookkeeping -- that's introduced in v1.8.
        return p;
    }
    void deallocate(void* a) {
        if (!a) return;

        // v1.6: bounds-checked free -- membership, block-alignment, and tail-padding checks.
        // Carried into v1.7 unchanged. Double-free of a *valid* address is NOT caught here
        // (that's the v1.8 in_use addition) -- a second deallocate() of the same legitimately-
        // freed block passes all three checks below and gets re-spliced onto freeListHead.
        bool found = false;
        for (size_t i = 0; i < memory_list.size(); ++i) {
            char* c = memory_list[i];
            if (a >= c && a < c + myChunkSize) {
                size_t off = (char*)a - c;
                if (off % blockSize == 0 && off < myChunkSize - 64) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) return;

        Node* r = (Node*)a;
        r->next = freeListHead;
        freeListHead = r;
    }
};

// ---- benchmark subject ----
struct Player {
    int id; int hp; int mp; std::string name;
};

static thread_local SimpleMemoryPool<Player> g_pool;

// ----- timer helper -----
static double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ===== single-thread: pool vs new/delete =====
// v1.9: `sink` is volatile and accumulates p->id after every construction. This is a
// standard-mandated observable side effect, so no compiler at any optimization level may
// eliminate the allocate/construct/destroy/free sequence as dead code. See file header
// comment for the GCC 8.5 (Linux) vs GCC 16.1 (Windows/MinGW) discrepancy this fixes.
static void bench_single(const char* label, bool use_pool, size_t iterations) {
    volatile long long sink = 0;
    double t0 = now_sec();
    if (use_pool) {
        for (size_t i = 0; i < iterations; ++i) {
            void* m = g_pool.allocate();
            Player* p = ::new (m) Player{ (int)i, 100, 50, "bench" };
            sink += p->id;
            p->~Player();
            g_pool.deallocate(p);
        }
    } else {
        for (size_t i = 0; i < iterations; ++i) {
            Player* p = new Player{ (int)i, 100, 50, "bench" };
            sink += p->id;
            delete p;
        }
    }
    double t1 = now_sec();
    double ops = iterations / (t1 - t0);
    // v1.9: printing sink (a) gives GCC an actual "use" of the volatile variable, silencing
    // -Wunused-but-set-variable (a separate diagnostic from the elision guarantee volatile
    // itself provides -- the warning just checks whether the value is ever read out anywhere),
    // and (b) doubles as a cheap correctness signal: if the loop didn't run, sink stays 0.
    std::printf("  [single] %-10s %10zu iters  %8.3f Mops/sec  (sink=%lld)\n", label, iterations, ops / 1e6, sink);
}

// ===== multi-thread: pool (per-thread TLS) vs raw malloc/free =====
// v1.9: same volatile-sink treatment as bench_single, applied to both worker functions for
// consistency — these didn't reproduce the "inf" bug in testing so far, but a thread-spawned
// function being harder for a compiler to elide today is not a guarantee it stays that way
// under a future/different compiler, so both are hardened the same way as bench_single.
static void worker_pool(size_t iters, std::atomic<size_t>* done) {
    volatile long long sink = 0;
    for (size_t i = 0; i < iters; ++i) {
        void* m = g_pool.allocate();
        Player* p = ::new (m) Player{ (int)i, 1, 2, "t" };
        sink += p->id;
        p->~Player();
        g_pool.deallocate(p);
    }
    done->fetch_add(1);
    (void)sink; // volatile writes above are never elided; this just silences -Wunused-but-set-variable
}
static void worker_sys(size_t iters, std::atomic<size_t>* done) {
    volatile long long sink = 0;
    for (size_t i = 0; i < iters; ++i) {
        Player* p = new Player{ (int)i, 1, 2, "t" };
        sink += p->id;
        delete p;
    }
    done->fetch_add(1);
    (void)sink; // volatile writes above are never elided; this just silences -Wunused-but-set-variable
}

static void bench_multi(const char* label, bool use_pool, unsigned threads, size_t iters_per_thread) {
    std::atomic<size_t> done{ 0 };
    double t0 = now_sec();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) {
        if (use_pool) ts.emplace_back(worker_pool, iters_per_thread, &done);
        else          ts.emplace_back(worker_sys,  iters_per_thread, &done);
    }
    for (auto& t : ts) t.join();
    double t1 = now_sec();
    size_t total = (size_t)threads * iters_per_thread;
    double ops = total / (t1 - t0);
    std::printf("  [multi ] %-10s %2u threads %10zu total  %8.3f Mops/sec\n", label, threads, total, ops / 1e6);
}

int main() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    // enough iterations that a single thread allocates > 1023 Players -> crosses the 64KB chunk
    // boundary and forces grow_pool() (exercises the v1.6 dynamic-growth path).
    size_t single_iters   = 50000000;          // 20000 Players > 1023 slots per 64KB chunk
    size_t multi_iters    = 2000000;          // per thread

    std::cout << "=== High-Performance Memory Pool - v1.7-baseline benchmark (no in_use) ===\n";
    std::cout << "hardware threads: " << hw << "\n\n";

    std::cout << "[A] single-threaded (forces cross-chunk growth)\n";
    bench_single("pool",   true,  single_iters);
    bench_single("system", false, single_iters);

    std::cout << "\n[B] multi-threaded (per-thread TLS pool vs system allocator)\n";
    bench_multi("pool",   true,  hw, multi_iters);
    bench_multi("system", false, hw, multi_iters);

    std::cout << "\n(done)\n";
    return 0;
}
