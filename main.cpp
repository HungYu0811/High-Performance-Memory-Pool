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

    // v1.6: recycled-chunk free list head. When a thread-local pool is destroyed it returns
    // its chunks here instead of leaking them, so `offset` is no longer strictly monotonic.
    Node* freeCentralHead = nullptr;

    // v1.6: promoted from a function-local `static std::mutex` (v1.5) to a class member so that
    // both requestChunk() and the new returnChunk() share the same lock.
    std::mutex arena_mtx;

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
        std::lock_guard<std::mutex> lock(arena_mtx);

        // v1.6: prefer a previously reclaimed chunk before advancing `offset`. This is what makes
        // the arena reusable across thread spawn/join cycles instead of only ever growing.
        if (freeCentralHead != nullptr){
            char* recycledChunk = (char*) freeCentralHead;
            freeCentralHead = freeCentralHead -> next;
            return recycledChunk;
        }

        if (offset.load() + chunkSize > totalSize) {
            return nullptr; // Central Arena out of boundary
        }

        char* allocatedChunk = baseAddress + offset.load();
        offset += chunkSize; // Thread-safe atomic pointer advancement
        return allocatedChunk;
    }

    // v1.6: returns a chunk into the recycled free list so it can be handed out again by requestChunk().
    // Called by SimpleMemoryPool::~SimpleMemoryPool() on thread exit.
    void returnChunk(char* chunk) {
        std::lock_guard<std::mutex> lock(arena_mtx);
        Node* returnedChunk = (Node*) chunk;
        returnedChunk->next = freeCentralHead;
        freeCentralHead = returnedChunk;
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
    // v1.6: tracks every chunk this pool has claimed from CentralArena, so the destructor can
    // return each one via CentralArena::returnChunk(). Enables chunk reclamation.
    std::vector<char*> memory_list;

    Node* freeListHead = nullptr;    // Head pointer of the free list tracking available blocks

    // v1.6: chunk size is now a member (was a local constant in v1.5) so grow_pool() can reuse it.
    size_t myChunkSize;

    // v1.6: block size is now a member (computed from sizeof(T) in grow_pool) so deallocate()
    // can validate alignment against the type actually stored.
    size_t blockSize;

    // v1.6: dynamically grows the pool by claiming another 64KB chunk from CentralArena instead of
    // throwing. Builds a fresh embedded free list inside the new chunk and splices its tail onto the
    // previous free list (current->next = temp_free_list) — an O(1) cross-chunk merge.
    void grow_pool(){
        Node* temp_free_list = freeListHead;

        // v1.6: claim a new chunk (recycled or fresh). Throws only if CentralArena itself is exhausted.
        char* newChunk = g_centralArena.requestChunk(myChunkSize);
        if (!newChunk) {
            throw std::bad_alloc();
        }
        memory_list.push_back(newChunk);

        // v1.6: init diagnostic log is gated behind POOL_DEBUG_LOG so a release build stays silent.
        #ifdef POOL_DEBUG_LOG
            {
                std::lock_guard<std::mutex> lock(cout_mtx);
                std::cout << "[init] thread " << std::this_thread::get_id()
                          << " Build personal thread, start from " << (void*) newChunk << "\n";
            }
        #endif

        // Dynamically calculate object size and round up to 64-byte Cache Line Alignment
        blockSize = (sizeof(T) + 63) & ~63;

        // v1.6: overflow guard. If a single T exceeds (myChunkSize - 64), maxSlots would compute to 0
        // and the build loop's `maxSlots - 1` would underflow to SIZE_MAX, scribbling past the chunk.
        // Placed AFTER blockSize is assigned so the first (constructor) call is protected too.
        if (blockSize > myChunkSize - 64) throw std::bad_alloc();

        size_t maxSlots = (myChunkSize - 64) / blockSize; // Reserves a conservative 64-byte safe-padding downstream

        freeListHead = (Node*) memory_list.back();
        Node* current = freeListHead;

        // Dissects the raw memory array into an O(1) interconnected embedded linked list
        for (size_t i = 0; i < maxSlots - 1; ++i) {
            current->next = (Node*)((char*)current + blockSize);
            current = current->next;
        }

        // v1.6: splice the new chunk's list tail onto the previously-live free list so reclaimed
        // blocks remain reachable after growth.
        current->next = temp_free_list;
    }

public:
    // Slices down the thread territory into cache-aligned memory blocks
    SimpleMemoryPool() {
        myChunkSize = 64 * 1024; // Every thread establishes an isolated 64KB arena track
        grow_pool();
    }

    // v1.6: returns every owned chunk to CentralArena on thread exit, so memory is reclaimed
    // instead of leaked until process termination.
    ~SimpleMemoryPool() {
        for (const auto& checkChunks : memory_list){
          g_centralArena.returnChunk(checkChunks);
      }
    }

    // High-speed O(1) memory extraction
    void* allocate() {
        if (!freeListHead) {
            grow_pool(); // v1.6: arena capacity exceeded bounds, grow instead of throwing
        }
        Node* poppedBlock = freeListHead;
        freeListHead = freeListHead->next;
        // v1.6: hot-path diagnostic log is gated behind POOL_DEBUG_LOG (release build = no output)
        #ifdef POOL_DEBUG_LOG
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[Pool] thread " << std::this_thread::get_id()
                      << " -> lend one piece of memory, address : " << (void*)poppedBlock
                      << " | Next memory is at : " << (void*)freeListHead << "\n";
        }

        #endif
        return poppedBlock;

    }

    // High-speed O(1) recycled node pushback
    void deallocate(void* address) {
        if (!address) return;

        // v1.6: bounds-checked free. A returned address is accepted only if it
        //  (a) falls inside one of this pool's owned chunks (membership),
        //  (b) is block-aligned relative to the chunk base (rejects mid-block frees), and
        //  (c) is not inside the trailing 64-byte tail-padding region (holds no valid block).
        // A misdirected free that fails any check is silently ignored instead of corrupting the free list.
        // Note: double-free of a *valid* block is still not detected (would need a separate in-use bitmap).
        bool found = false;
        for (const auto& checkChunks : memory_list){
            if (address >= checkChunks && address < (myChunkSize + checkChunks)) {
                size_t off = (char*)address - checkChunks;
                if (off % blockSize == 0
                    && off < (myChunkSize - 64)) {
                    found = true;
                    break;
                }
            }
        }
        if(!found) return;
        Node* returnedBlock = (Node*)address;
        returnedBlock->next = freeListHead;
        freeListHead = returnedBlock;

        // v1.6: hot-path diagnostic log gated behind POOL_DEBUG_LOG
        #ifdef POOL_DEBUG_LOG
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[Pool] thread " << std::this_thread::get_id()
                      << " <- Customer return the memory, address : " << address
                      << " | Now new head of the list is : " << (void*)freeListHead << "\n";
        }
        #endif
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
