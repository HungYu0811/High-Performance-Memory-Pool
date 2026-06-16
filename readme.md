# High-Performance Memory Pool

* **Project:** High-Performance-Memory-Pool
* **Author:** HungYu
* **Date:** 2026-06-16

---

## Project Overview

Verify how to implement a high-performance, O(1) complexity, and completely lock-free custom memory allocator in a multi-threaded environment using C++ memory management techniques.

---

## Architecture Evolution & Modify

### v1 : Fixed-Size Memory Pool with link list ('Commit_ver1')
* **Design:** Allocated raw memory via 'new char[96]' and sliced it into three 32-byte blocks, chained as a Linked List.
* **Purpose:** Verified the core logic of allocating and deallocating memory blocks using pointer arithmetic.

### v2 : Add UserInfo ('Commit_ver2')
* **Design:** Added 'UserInfo' structure inside the 'Player' object.
* **Purpose:** Simulate a realistic game entity scenario where data size grows.

---

### v3 : Override global new/delete operators and implement network interception ('Commit_ver3')
* **Design:** Defined a global 'SimpleMemoryPool* global_pool' and overloaded the global 'void* operator new(size_t size)' and 'void operator delete(void* address)'.

#### Encountered Bottlenecks:
1. **Multithread bottleneck:** All CPU cores simultaneously compete for the same global pool.
2. **The "Phantom 3rd Delete" (Self-Interception):** After 'main()' ended, the log unexpectedly tracked a 3rd delete pointing to the pool's base address. This happened because inside '~SimpleMemoryPool()', the 'delete[] rawMemory;' code inadvertently triggered our global delete hook, causing the pool to intercept its own destruction phase.
3. **The Character Alignment and Size Discrepancy:** In earlier stages, using 'char name[20]' led to unpredictable compiler data padding and alignment issues within the 'Player' struct, causing the actual object footprint to exceeding our strict 32-byte memory block boundary. To standardize the memory layout and eliminate manual array offset bugs, we migrated the field to 'std::string name'. This made the alignment predictable but expanded the Player size to a fixed 48 bytes, forcing a pool redesign.

#### The Solution & Refactoring:
* To bypass the global C++ delete network during cleanup, we had to use C-style 'std::free(rawMemory)' in the destructor. 
* To strictly follow C++ memory pairing rules (preventing Undefined Behavior caused by mixing 'new[]' with 'free'), we also refactored the constructor's allocation from 'new char[96]' to 'std::malloc(96)'. 
* This 'malloc'/'free' pairing successfully eliminated the phantom 3rd delete log.

---

### v4: Class-Specific Thread-Local 64-Byte Allocator ('Commit_ver4')

#### Design:
1. **Thread-Local Isolation:** Changed 'global_pool' to 'thread_local local_pool'. Each CPU core now runs on its own independent memory track. No mutex, no queueing, 100% lock-free.
2. **Class-Specific Overload:** Moved 'operator new/delete' directly inside 'struct Player'. Only 'Player' allocations touch our pool, preventing standard library components from triggering recursion bugs.

#### The Crucial Fix (Solving the 48-byte Overflow):
* **The Problem:** In v3, switching to 'std::string' expanded the 'Player' object size to 48 bytes. However, our old pool was still slicing blocks at 32-byte intervals. Writing a 48-byte object into a 32-byte space caused a critical memory overflow, overwriting and destroying the 'Node* next' pointer of the adjacent block.
* **The Solution:** Upgraded the block slicing size from 32 bytes to 64 bytes. This extra padding completely contains the 48-byte 'Player' object and ensures total memory safety.

---

## Verification & Results

Confirmed via Linux terminal output:
1. **Independent Addresses:** Core '[0]', '[1]', and '[2]' print completely different memory address ranges (e.g., '0x7fd45c000b80' vs '0x7fd454000b80'), proving 'thread_local' isolation works perfectly.
2. **Clean Pointers:** The 'Next tofu is at' logs show perfectly clean aligned hex addresses (e.g., '0x...c00') instead of being corrupted by string data leftovers.
3. **True O(1) Complexity:** Zero mutex overhead, maximum multi-core throughput.