#include <iostream>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
using namespace std;

// Synchronizes terminal stdout to prevent multi-threaded text overlapping
std::mutex cout_mtx;

/**
 * Embedded Pointer Structure
 * Rationale: When a memory block is inactive (free), its first 8 bytes are repurposed
 * to store the memory address of the next free slot.
 * Advantage: Implements an implicit linked list inside the arena without incurring
 * any additional metadata or per-block structural overhead.
 */
struct Node {
    Node* next;
};

/**
 * Two-Stage Central Arena Allocator (Vacuum Zone Allocation)
 * Rationale:
 * Requests a massive contiguous OS virtual address range upon initialization via OS native APIs.
 * Distributes large chunks to individual thread-local pools dynamically, bypassing heavy kernel context switches later.
 */
class CentralArena {
private:
    char* baseAddress;
    size_t totalSize;
    atomic<size_t> offset; // Tracks current global offset bounded inside the virtual space

public:
    CentralArena(size_t size) : totalSize(size), offset(0) {
#ifdef _WIN32
        // Windows Native Memory API Allocation
        baseAddress = (char*)VirtualAlloc(NULL, totalSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        // POSIX mmap Native Mapping Interface (Anonymous & Private Track)
        baseAddress = (char*)mmap(NULL, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (!baseAddress) {
            std::cerr << "[CRITICAL ERROR] Failed to allocate global Vacuum Zone Virtual Space!\n";
        }
    }

    // Segregates and slices out dedicated territories for incoming thread-local pool streams
    char* requestChunk(size_t chunkSize) {
        // Mutex is strictly localized to thread bootstrap initialization phase. Zero-lock during runtime loops.
        static std::mutex arena_mtx;
        std::lock_guard<std::mutex> lock(arena_mtx);

        if (offset.load() + chunkSize > totalSize) {
            return nullptr; // Central Arena out of boundary
        }
        
        char* allocatedChunk = baseAddress + offset.load();
        offset += chunkSize; // Thread-safe atomic pointer advancement
        return allocatedChunk;
    }

    ~CentralArena() {
#ifdef _WIN32
        VirtualFree(baseAddress, 0, MEM_RELEASE);
#else
        munmap(baseAddress, totalSize);
#endif
    }
};

// Global Central Arena Singleton Instance - Allocating 400MB Virtual Buffer Space
CentralArena g_centralArena(400 * 1024 * 1024);

/**
 * Fixed-Size Memory Pool (Arena Allocator)
 * Rationale:
 * 1. Pre-allocates a continuous chunk of heap memory to eliminate frequent runtime
 * system calls (malloc), reducing context switches and kernel-mode transitions.
 * 2. Manages memory distribution via a Free List with O(1) time complexity.
 */
class SimpleMemoryPool {
private:
    char* rawMemory;       // Start address of the segregated 64KB continuous territory
    Node* freeListHead;    // Head pointer of the free list tracking available blocks

public:
    // Slices down the thread territory into cache-aligned memory blocks
    SimpleMemoryPool() {
        size_t myChunkSize = 64 * 1024; // Every thread establishes an isolated 64KB arena track
        rawMemory = g_centralArena.requestChunk(myChunkSize);

        if (!rawMemory) {
            throw std::bad_alloc();
        }
        
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[init] thread " << std::this_thread::get_id()
                      << " Build personal thread, start from " << (void*)rawMemory << "\n";
        }
        
        size_t blockSize = 64;
        size_t maxSlots = (myChunkSize - 64) / blockSize; // Reserves a conservative 64-byte safe-padding downstream

        freeListHead = (Node*)rawMemory;
        Node* current = freeListHead;

        // Dissects the raw memory array into an O(1) interconnected embedded linked list
        for (size_t i = 0; i < maxSlots - 1; ++i) {
            current->next = (Node*)((char*)current + blockSize);
            current = current->next;
        }
        current->next = nullptr;
    }

    ~SimpleMemoryPool() {}

    // High-speed O(1) memory extraction
    void* allocate() {
        if (!freeListHead) {
            throw std::bad_alloc(); // Arena capacity exceeded bounds
        }
        Node* poppedBlock = freeListHead;
        freeListHead = freeListHead->next; 
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[Pool] thread " << std::this_thread::get_id()
                      << " -> lend one piece of memory, address : " << (void*)poppedBlock
                      << " | Next memory is at : " << (void*)freeListHead << "\n";
        }
        return poppedBlock;
    }

    // High-speed O(1) recycled node pushback
    void deallocate(void* address) {
        if (!address) return;
        Node* returnedBlock = (Node*)address;
        returnedBlock->next = freeListHead; 
        freeListHead = returnedBlock;
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[Pool] thread " << std::this_thread::get_id()
                      << " <- Customer return the memory, address : " << address
                      << " | Now new head of the list is : " << (void*)freeListHead << "\n";
        }
    }
};

/**
 * Thread-Local Storage (TLS) Strategy
 * Architectural Impact (Critical):
 * Instantiates an isolated static object instance of the Memory Pool for every executing thread.
 * Rationale:
 * 1. Confines allocations to a thread's local storage boundary to bypass all lock primitives (Lock-Free).
 * 2. Declaring as an object instead of a pointer guarantees that the compiler automatically invokes
 * the destructor (~SimpleMemoryPool) upon thread destruction, establishing a closed lifecycle with zero leaks.
 * 3. Leverages Lazy Initialization automatically provided by the C++ TLS runtime spec.
 */
thread_local SimpleMemoryPool local_pool;

struct Player {
    int id;
    int hp;
    int mp;
    string name;

    void print_info(int core_id) {
        lock_guard<mutex> lock(cout_mtx);
        cout << "[Player Status] CPU Core [" << core_id << "] -> "
             << name << " (ID: " << id
             << ", HP: " << hp
             << ", MP: " << mp << ") | Memory Address: " << this << "\n";
    }
    
    /**
     * Operator new Overload
     * Intercepts standard runtime heap routing. Instead of calling global new,
     * it reroutes to the calling thread's local TLS pool instance.
     */
    static void* operator new(size_t size) {
        return local_pool.allocate();
    }

    /**
     * Operator delete Overload
     * Safely routes memory addresses directly back into the originating TLS pool object scope.
     */
    static void operator delete(void* address) noexcept {
        if (address != nullptr) {
            local_pool.deallocate(address);
        }
    }
};

// Worker entry point representing independent CPU Execution Cores
void game_core_worker(int core_id) {
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << ">>> CPU Core [ " << core_id << "] Start execution game logic...\n";
    }
    
    // Memory metrics bound directly within the aligned virtual segments
    Player* p1 = new Player{core_id, 100, 50, "Leo"};
    p1->print_info(core_id);
    Player* p2 = new Player{core_id + 10, 200, 80, "Vicky"};
    p2->print_info(core_id);
    
    // Fully unlocked high performance deletion pipelines
    delete p1;
    delete p2;
    
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "<<< CPU Core [" << core_id << "] Execution complete.\n";
    }
}

int main() {
    cout << "=== Start Game Server, prepare 3 CPU Cores ===\n\n";
    vector<thread> cpu_cores;
    
    // Distribute workloads concurrently across multiple runtime threads
    for(int i = 0; i < 3; ++i) {
        cpu_cores.push_back(thread(game_core_worker, i));
    }
    
    // Join threads to block until execution completes, reclaiming OS resources
    for (auto& core : cpu_cores) {
        core.join();
    }
    
    cout << "\n=== All core finish, perfect thread-local no lock distribute success ! ===\n";
    return 0;
}
