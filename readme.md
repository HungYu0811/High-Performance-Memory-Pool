# High-Performance Memory Pool

* **Project:** High-Performance-Memory-Pool
* **Author:** HungYu
* **Date:** 2026-06-20

---

## Project Overview

Verify how to implement a high-performance, O(1) complexity, and completely lock-free custom memory allocator in a multi-threaded environment using C++ memory management techniques.

---

## Architecture Evolution & Modify

### v1.0 : Fixed-Size Memory Pool with link list 
* **Design:** Allocated raw memory via 'new char[96]' and sliced it into three 32-byte blocks, chained as a Linked List.
* **Purpose:** Verified the core logic of allocating and deallocating memory blocks using pointer arithmetic.

### v1.1 : Add UserInfo 
* **Design:** Added 'UserInfo' structure inside the 'Player' object.
* **Purpose:** Simulate a realistic game entity scenario where data size grows.

---

### v1.2 : Override global new/delete operators and implement network interception 
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

### v1.3: Class-Specific Thread-Local 64-Byte Allocator with Safety Margin

#### Design:
1. **Thread-Local Isolation (Deterministic Lifecycle):** Migrated from a global pool pointer to a direct object instance: 'thread_local SimpleMemoryPool local_pool;'. Each CPU core now runs on its own independent memory track with zero lock contention. Furthermore, declaring it as an object instance guarantees that '~SimpleMemoryPool()' is automatically invoked upon thread destruction, achieving a closed lifecycle with zero memory leaks.
2. **Class-Specific Overload:** Moved 'operator new/delete' directly inside 'struct Player'. Only 'Player' allocations touch our pool, preventing standard library components (like 'std::string') from triggering cascading allocation loop bugs.

#### The Crucial Fix (Solving the 48-byte Overflow & Boundary Margin):
* **The Problem:** In v1.2, switching to 'std::string' expanded the 'Player' object size to 48 bytes. Slicing blocks at 32-byte intervals caused critical memory corruption, as object data overran into adjacent blocks and obliterated the 'Node* next' embedded pointers.
* **The Solution:** Upgraded the block slicing size from 32 bytes to 64 bytes (matching hardware Cache Line Alignment to maximize L1/L2 cache efficiency). Concurrently, expanded the total arena allocation size to 224 bytes ('64 bytes * 3 slots + 32 bytes tail padding'). The extra 32 bytes act as a strict downstream safety boundary margin, completely shielding the application from out-of-bounds undefined behavior during high-tier compiler optimization ('-O3').

---

### v1.4: Two-Stage Central Arena Allocator via OS Native Virtual Memory Management

#### Design:
1. **OS Native Virtual Space Reservation (Vacuum Zone Allocation):** Completely stripped out standard runtime intermediate allocation ('std::malloc'). Created a centralized singleton 'CentralArena' that immediately claims a massive 400MB contiguous virtual address space upon application boot using OS-native system calls ('VirtualAlloc' on Windows / 'mmap' on POSIX). This bypasses the traditional heap allocation middleware and grants absolute memory control directly from the OS kernel.
2. **Two-Stage Slicing with Sequential Thread Isolation:** When an independent CPU execution thread initializes its 'thread_local SimpleMemoryPool', it no longer talks to the OS or C-runtime heap. Instead, it requests a dedicated 64KB sub-territory from the 'CentralArena'.
3. **Thread-Safe Atomic Boundary Tracing:** To guard the initial thread bootstrap phase against concurrency race conditions, the global allocation pointer inside 'CentralArena' is tracked via 'std::atomic<size_t> offset'. This ensures that thread territories are sliced sequentially without overlap, while the actual runtime game logic ('new'/'delete' loops) remains completely lock-free.
4. **Unified Native Lifecycle Logging:** Refactored runtime diagnostic logs to utilize 'std::this_thread::get_id()', allowing real-time cross-examination of kernel-level thread dispatch scheduling alongside raw address structures.

---

## Verification & Results 

Confirmed via Linux terminal output ('g++ main.cpp -o main -pthread'):
1. **Thread-Local Storage Isolation:** Core '[0]', '[1]', and '[2]' log entirely disjoint memory address ranges (e.g., '0x7f2e68000b60' vs '0x7f2e70000b60'), proving complete execution thread isolation under heavy core loads.
2. **Perfect Offset Math:** The allocation delta between adjacent chunks (e.g., '0x7f2e68000ba0' - '0x7f2e68000b60') calculates exactly to '0x40' (64 bytes in decimal), verifying precision pointer arithmetic and cache line alignment.
3. **Implicit Free List Circular Lifecycle:** The log tracks returned memory addresses instantly becoming the new head of the implicit list. This proves the O(1) push-front recycling mechanism works flawlessly, creating a highly sustainable memory reuse loop.
4. **OS Page-Boundary Alignment Validation:** Terminal execution metrics in v1.4 reveal that every single thread pool base segment initializes strictly at an OS virtual memory page boundary limit (verified by hexadecimal pointer values terminating cleanly in '0x000' page fractions, e.g., '0x7f7992aa3000'). This completely eliminates hardware address shifting overhead, accelerating TLB cache lookups to the silicon limit.
5. **Sequential Atomic Segmentation Proof:** Address allocation gaps between competing threads exhibit perfect '0x10000' interval strides (exactly 64KB increments, tracking linearly as '0x...a3000' ➔ '0x...b3000' ➔ '0x...c3000'). This data confirms the 'std::atomic' offset manager successfully enforces strict spatial segregation across CPU cores without triggering secondary cross-core cache invalidation.