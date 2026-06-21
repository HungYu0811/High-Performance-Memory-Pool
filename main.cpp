#include <iostream>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <new>
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
 * Template-Based Fixed-Size Memory Pool
 * Automatically scales block sizes to match hardware Cache Line limits based on DataType.
 */
template <typename T>
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
        //Dynamically calculate object size and round up to 64-byte Cache Line Alignment
        size_t blockSize = (sizeof(T) + 63) & ~63; 
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
 * User Network Request Simulation Packet
 */
struct UserRequest {
    int id;
    int hp;
    int mp;
    string name;
};
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
    
};

// Instantiating the generic pool explicitly bound to the Player structure size metrics
thread_local SimpleMemoryPool<Player> local_player_pool;

/**
 * Dynamic Worker Loop
 * Processes variable workload streams dynamically according to vector request payloads.
 */
void game_core_worker(int core_id, std::vector<UserRequest> requests) {
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << ">>> CPU Core [ " << core_id << "] Processing " << requests.size() << " user packets...\n";
    }
    
    std::vector<Player*> active_players;

    // Dynamically constructing arbitrary numbers of entities requested by the user flow
    for (const auto& req : requests) {
        // Step 1: Claim raw cache-aligned slot from TLS pool
        void* mem = local_player_pool.allocate();
        
        // Step 2: Placement new runtime execution - maps runtime variables on memory precisely
        Player* p = ::new (mem) Player{req.id, req.hp, req.mp, req.name};
        
        p->print_info(core_id);
        active_players.push_back(p);
    }
    
    // Controlled destruction tear-down loop
    for (auto* p : active_players) {
        p->~Player();                  // Manually release internal heap assets (std::string inner buffer)
        local_player_pool.deallocate(p); // Recycle back to the local free list
    }
    
    {
        lock_guard<mutex> lock(cout_mtx);
        cout << "<<< CPU Core [" << core_id << "] Workload processed successfully.\n";
    }
}
int main() {
    cout << "=== Start Dynamically Scaled Game Server, Preparing Cores ===\n\n";
    
    // 💡 Simulating dynamic user load profiles on different cores
    std::vector<UserRequest> core0_payload = {
        {101, 100, 50, "Dynamic_Leo"},
        {102, 120, 60, "Dynamic_Vicky"}
    };

    std::vector<UserRequest> core1_payload = {
        {201, 999, 888, "Boss_Sephiroth"},
        {202, 150, 200, "Mage_Aerith"},
        {203, 300, 100, "Warrior_Cloud"} // Core 1 dynamically requests 3 players!
    };

    std::vector<UserRequest> core2_payload = {
        {301, 50, 10, "Goblin_A"} // Core 2 dynamically requests only 1 monster!
    };

    vector<thread> cpu_cores;
    
    // Dispatching variable task bundles across individual pipeline workers
    cpu_cores.push_back(thread(game_core_worker, 0, core0_payload));
    cpu_cores.push_back(thread(game_core_worker, 1, core1_payload));
    cpu_cores.push_back(thread(game_core_worker, 2, core2_payload));
    
    for (auto& core : cpu_cores) {
        core.join();
    }
    
    cout << "\n=== All dynamic payloads processed successfully under pure lock-free isolation! ===\n";
    return 0;
}

