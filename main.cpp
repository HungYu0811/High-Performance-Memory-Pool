#include <iostream>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
using namespace std;
//To prevent multithread cout courrency problem
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
 * Fixed-Size Memory Pool (Arena Allocator)
 * Rationale:
 * 1. Pre-allocates a continuous chunk of heap memory to eliminate frequent runtime 
 * system calls (malloc), reducing context switches and kernel-mode transitions.
 * 2. Manages memory distribution via a Free List with O(1) time complexity.
 */
class SimpleMemoryPool {
private:
    char* rawMemory;       // Start address of the pre-allocated continuous memory block     
    Node* freeListHead;    // Head pointer of the free list tracking available blocks

public:
    // Constructor: Requests the memory arena from the OS and segments it into aligned slots
    SimpleMemoryPool() {
        /**
         * Rationale for Arena Sizing:
         * Allocates 224 bytes total (64 bytes * 3 slots + 32 bytes tail padding).
         * The extra 32 bytes serve as a safety boundary margin (Padding) at the end of the arena.
         * This prevents out-of-bounds undefined behavior or memory corruption during compiler-driven 
         * optimization (-O3) or execution state checking.
         */
        rawMemory = reinterpret_cast<char*>(std::malloc(64 * 3 + 32));

        // Segmenting the arena using Pointer Arithmetic.
        // Slots are sized at 64 bytes (power-of-two alignment) to ensure they map 
        // cleanly to hardware Cache Lines (64-byte width), preventing false sharing 
        // and maximizing L1/L2 cache efficiency.

        Node* block1 = reinterpret_cast<Node*>(rawMemory);       // Offset: 0
        Node* block2 = reinterpret_cast<Node*>(rawMemory + 64);  // Offset: 64
        Node* block3 = reinterpret_cast<Node*>(rawMemory + 128); // Offset: 128

        // Construct the structural chain of the implicit free list
        block1->next = block2;
        block2->next = block3;
        block3->next = nullptr; 

        freeListHead = block1;
        lock_guard<mutex> lock(cout_mtx);
        cout << "[init] thread " << std::this_thread::get_id()<< " Build personal thread, start from " << (void*) freeListHead << endl;
    }
/**
 * Allocation Interface
 * Time Complexity: O(1)
 * Rationale: Pops the head node from the free list and hands the raw memory block 
 * over to the application layer.
 */
    void* allocate() {
        if (freeListHead == nullptr) {
            lock_guard<mutex> lock(cout_mtx);
            cout << "[Error] thread   run out of memroy" << endl;
            return nullptr;
        }

        Node* chunkToBorrow = freeListHead;

        freeListHead = freeListHead->next; // Move the free list head forward
        lock_guard<mutex> lock(cout_mtx);
        cout << "[Pool] thread "<< std::this_thread::get_id() <<" -> lend one peace of memory, address : " << chunkToBorrow 
                  << " | Next memory is at : " << freeListHead << "\n";
        
        return reinterpret_cast<void*>(chunkToBorrow);
    }
/**
 * Deallocation Interface
 * Time Complexity: O(1)
 * Rationale: Casts the returned address back to a structural Node and performs 
 * a Push Front operation to insert it back into the free list instantly.
 */
    void deallocate(void* address) {
        if (address == nullptr) return;

        Node* returnedChunk = reinterpret_cast<Node*>(address);

        returnedChunk->next = freeListHead;   // Link the old head to the new node's next
        
        freeListHead = returnedChunk;         // Update the head pointer to the returned node
        lock_guard<mutex> lock(cout_mtx);

        cout << "[Pool] thread " << std::this_thread::get_id() <<  " <- Customer return the memory, address : " << address 
                  << " | Now new head of the list is : " << freeListHead << "\n";
    }
    // Destructor: Recycles the entire raw arena to avoid severe system memory leaks
    ~SimpleMemoryPool() {
       free(rawMemory);
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
struct Player{
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
void game_core_worker(int core_id){
  {
    lock_guard<mutex> lock(cout_mtx);
    cout << ">>> CPU Core [ " << core_id << "] Start execution game logic...\n";
  }
    // Allocation hits the overloaded new operator, leveraging high-speed TLS memory slices
    Player* p1 = new Player{core_id, 100, 50, "Leo"};
    p1->print_info(core_id);
    Player* p2 = new Player{core_id + 10, 200, 80, "Vicky"};
    p2->print_info(core_id);
    // Deallocation invokes the local allocator's custom recycle pipeline, zero locks required
    delete p1;
    delete p2;
  {
    lock_guard<mutex> lock(cout_mtx);
    cout << "<<< CPU Core [" << core_id << "] Execution complete。\n";
  }
}
int main() {
    cout << "===Start Game Server, prepare 3 CPU Cores ===\n\n";
    vector<thread> cpu_cores;
    // Distribute workloads concurrently across multiple runtime threads
    for(int i = 0; i < 3; ++i){
      cpu_cores.push_back(thread(game_core_worker, i) );
    }
    // Join threads to block until execution completes, reclaiming OS resources
    for (auto& core : cpu_cores) {
        core.join();
    }
    cout << "\n=== All core finish, perfect thread-local no lock distribute success ! ===\n";  
    return 0;
}
