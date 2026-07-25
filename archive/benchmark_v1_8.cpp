// v1.8 benchmark: measures the pool allocator against the system allocator
// (std::malloc / default operator new) under single- and multi-threaded load,
// and deliberately exceeds a single 64KB chunk per thread to exercise the
// v1.6 dynamic-growth path.
//
// v1.8 change from the v1.7 benchmark: SimpleMemoryPool below now carries the
// same in_use/maxSlots/double-free-detection logic as main.cpp, so this
// benchmark measures the actual v1.8 pool (including the double-free check's
// hot-path cost), not the older v1.6-era pool. This lets v1.8's numbers be
// compared directly against the v1.7 numbers already recorded in the README
// to quantify the cost of double-free detection.
//
// v1.9 changes (two, independent, both about benchmark/pool correctness):
//
// 1. in_use switched from std::vector<bool> to std::vector<uint8_t> (same change as
//    main.cpp — see that file's v1.9 comments for the full rationale: vector<bool> is
//    bit-packed, costing extra shift/mask work per access; block count per chunk is small
//    enough that the 8x memory cost of one byte per block is a good trade for hot-path speed).
//
// 2. `volatile long sink` added to bench_single/worker_pool/worker_sys. Discovered while
//    cross-testing this exact file on Windows (MinGW-w64 GCC 16.1.0) vs Linux (GCC 8.5.0):
//    the `system` (new/delete) single-threaded loop was silently optimized away entirely on
//    the newer compiler, producing a nonsensical "inf Mops/sec" result. Root cause: `Player{
//    ..., "bench" }` never escapes the loop body and is never read before being destroyed,
//    and "bench" (5 chars) fits inside std::string's small-string-optimization buffer so
//    construction never touches the heap either — the whole allocate/construct/destroy/free
//    sequence has no observable side effect, so the C++ standard permits a compiler to elide
//    it entirely (allocation elision). GCC 8.5 didn't perform this optimization on this
//    pattern; GCC 16.1 did. This was a real risk on Linux too (glibc malloc being an opaque
//    external call happened to make GCC 8.5 not attempt it here — that's not a guarantee it
//    never will on a newer glibc/GCC pairing). `sink += p->id` after each construction is a
//    volatile write, which is a defined observable side effect under the standard, so no
//    compiler at any optimization level is permitted to eliminate the loop. Both the pool
//    and system code paths get the same treatment for a fair comparison.
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

// v1.8: in_use/maxSlots added below, matching main.cpp exactly, so this
// benchmark's allocate()/deallocate() pay the same double-free-detection
// cost as the real pool.
// NOTE: this file is the v1.8 baseline for interleaved A/B testing against v1.9's
// benchmark.cpp. The ONLY intentional difference from benchmark.cpp is in_use's type
// (vector<bool> here vs vector<uint8_t> there) — everything else, including the v1.9
// volatile-sink dead-code-elimination fix and the long->long long fix, is kept IDENTICAL
// on purpose, so that an interleaved test isolates the vector<bool> vs vector<uint8_t>
// variable cleanly instead of also mixing in the elision-bug fix as a second variable.
template <typename T>
class SimpleMemoryPool {
private:
    std::vector<char*> memory_list;
    std::vector<bool> in_use;
    Node* freeListHead = nullptr;
    size_t myChunkSize;
    size_t blockSize;
    size_t maxSlots;

    void grow_pool() {
        Node* temp = freeListHead;
        char* newChunk = g_centralArena.requestChunk(myChunkSize);
        if (!newChunk) throw std::bad_alloc();
        memory_list.push_back(newChunk);

        blockSize = (sizeof(T) + 63) & ~63;
        if (blockSize > myChunkSize - 64) throw std::bad_alloc();

        maxSlots = (myChunkSize - 64) / blockSize;
        for (size_t i = 0; i < maxSlots; ++i) in_use.push_back(false);

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

        // v1.8: mark this block in-use (same logic as main.cpp).
        for (size_t i = 0; i < memory_list.size(); ++i) {
            char* c = memory_list[i];
            if ((char*)p >= c && (char*)p < c + myChunkSize) {
                size_t off = (char*)p - c;
                size_t local_index = off / blockSize;
                in_use[i * maxSlots + local_index] = true;
                break;
            }
        }
        return p;
    }
    void deallocate(void* a) {
        if (!a) return;
        bool found = false;
        size_t chunk_number = 0;
        for (size_t i = 0; i < memory_list.size(); ++i) {
            char* c = memory_list[i];
            if (a >= c && a < c + myChunkSize) {
                size_t off = (char*)a - c;
                if (off % blockSize == 0 && off < myChunkSize - 64) {
                    found = true;
                    chunk_number = i;
                    break;
                }
            }
        }
        if (!found) return;

        // v1.8: double-free check (same logic as main.cpp).
        size_t off = (char*)a - memory_list[chunk_number];
        size_t local_index = off / blockSize;
        size_t index = chunk_number * maxSlots + local_index;
        if (!in_use[index]) return;

        Node* r = (Node*)a;
        r->next = freeListHead;
        freeListHead = r;
        in_use[index] = false;
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

    std::cout << "=== High-Performance Memory Pool - v1.8-baseline benchmark (vector<bool>) ===\n";
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
