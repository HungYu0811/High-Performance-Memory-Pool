// v1.7 benchmark: measures the pool allocator against the system allocator
// (std::malloc / default operator new) under single- and multi-threaded load,
// and deliberately exceeds a single 64KB chunk per thread to exercise the
// v1.6 dynamic-growth path.
//
// Build:  cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
// Run:    ./benchmark            (or benchmark.exe on Windows)
//
// Output is a plain table of operations/sec. No logging (POOL_DEBUG_LOG off in Release).

#include <iostream>
#include <cstddef>
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
        size_t maxSlots = (myChunkSize - 64) / blockSize;
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
        blockSize = (sizeof(T) + 63) & ~63;
        grow_pool();
    }
    ~SimpleMemoryPool() {
        for (auto c : memory_list) g_centralArena.returnChunk(c);
    }
    void* allocate() {
        if (!freeListHead) grow_pool();
        Node* p = freeListHead;
        freeListHead = freeListHead->next;
        return p;
    }
    void deallocate(void* a) {
        if (!a) return;
        bool found = false;
        for (auto c : memory_list) {
            if (a >= c && a < c + myChunkSize) {
                size_t off = (char*)a - c;
                if (off % blockSize == 0 && off < myChunkSize - 64) { found = true; break; }
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
static void bench_single(const char* label, bool use_pool, size_t iterations) {
    double t0 = now_sec();
    if (use_pool) {
        for (size_t i = 0; i < iterations; ++i) {
            void* m = g_pool.allocate();
            Player* p = ::new (m) Player{ (int)i, 100, 50, "bench" };
            p->~Player();
            g_pool.deallocate(p);
        }
    } else {
        for (size_t i = 0; i < iterations; ++i) {
            Player* p = new Player{ (int)i, 100, 50, "bench" };
            delete p;
        }
    }
    double t1 = now_sec();
    double ops = iterations / (t1 - t0);
    std::printf("  [single] %-10s %10zu iters  %8.3f Mops/sec\n", label, iterations, ops / 1e6);
}

// ===== multi-thread: pool (per-thread TLS) vs raw malloc/free =====
static void worker_pool(size_t iters, std::atomic<size_t>* done) {
    for (size_t i = 0; i < iters; ++i) {
        void* m = g_pool.allocate();
        Player* p = ::new (m) Player{ (int)i, 1, 2, "t" };
        p->~Player();
        g_pool.deallocate(p);
    }
    done->fetch_add(1);
}
static void worker_sys(size_t iters, std::atomic<size_t>* done) {
    for (size_t i = 0; i < iters; ++i) {
        Player* p = new Player{ (int)i, 1, 2, "t" };
        delete p;
    }
    done->fetch_add(1);
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
    size_t single_iters   = 20000;          // 20000 Players > 1023 slots per 64KB chunk
    size_t multi_iters    = 50000;          // per thread

    std::cout << "=== High-Performance Memory Pool — v1.7 benchmark ===\n";
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
