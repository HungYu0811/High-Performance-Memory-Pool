# High-Performance Memory Pool

* **Project:** High-Performance-Memory-Pool
* **Author:** HungYu
* **Date:** 2026-07-08
* **Status:** Work in progress / learning project — see [Known Limitations](#known-limitations) below.

---

## Project Overview

An exploration of how to implement a custom, O(1) complexity, thread-aware memory allocator in a multi-threaded C++ environment. The hot-path allocate/deallocate loop is lock-free; a mutex is still used on the cold path (one-time per-thread arena bootstrap). See version notes below for exactly where locking does and doesn't apply.

---

## Architecture Evolution & Modify

### v1.0 : Fixed-Size Memory Pool with Linked List
* **Design:** Allocated raw memory via `new char[96]` and sliced it into three 32-byte blocks, chained as a linked list using an embedded `Node* next` pointer inside each free block.
* **Purpose:** Verified the core logic of allocating and deallocating memory blocks using pointer arithmetic (push-front / pop-front on the free list).
* **Note:** No alignment guarantees on the sliced blocks (relies on `new[]` returning sufficiently aligned memory, which holds in practice but isn't explicitly enforced). Uses `reinterpret_cast<Node*>` between `char*` and `Node*`, which is common practice but technically outside strict aliasing rules.

### v1.1 : Add Player Structure
* **Design:** Added a `Player` structure (`id`, `hp`, `mp`, `std::string name`) and used placement `new` to construct `Player` objects inside pool-allocated blocks.
* **Purpose:** Simulate a realistic game entity scenario where object size grows beyond a trivial POD.
* **⚠️ Retrospective note:** This version silently introduced a block-overflow bug. `sizeof(Player)` (dominated by `std::string`, typically ~32 bytes on 64-bit libstdc++ plus the three `int` fields and padding) exceeds the pool's 32-byte block size. Placement-`new`-ing a `Player` into a 32-byte block overwrites part of the adjacent block, including its embedded `Node* next` pointer — undefined behavior that happened not to crash in this demo because only 2–3 blocks were exercised and the corrupted bytes weren't read back before being overwritten again. This bug was not detected until v1.3, when the block size was deliberately increased to 64 bytes.

### v1.2 : Override Global `operator new` / `operator delete`
* **Design:** Defined a global `SimpleMemoryPool* global_pool` and overrode the global `operator new(size_t)` / `operator delete(void*)`, routing allocations ≤ 32 bytes to the pool.

#### Encountered Bottlenecks:
1. **Multithread bottleneck:** All CPU cores would compete for the same global pool (no thread isolation yet).
2. **Program-wide interception risk:** Because `operator new`/`operator delete` were overridden globally (not scoped to `Player`), *every* allocation in the program — including internal STL allocations — passed through this hook and its `size <= 32` check. Any unrelated allocation that happened to be ≤ 32 bytes could be misrouted into the pool, which has no way to know the memory didn't actually hold a `Player`. This is a broader risk than just "multithread contention," and is one of the motivations for the class-specific override in v1.3.
3. **The "Phantom 3rd Delete" (Self-Interception):** Inside `~SimpleMemoryPool()`, `delete[] rawMemory;` triggered the same global `operator delete` hook, causing the pool to intercept its own teardown and log an unexpected 3rd delete pointing at the pool's base address.
4. **Size/alignment discrepancy carried over from v1.1:** `sizeof(Player)` needs to be verified against the actual block size on the build platform — this was not directly measured in code (no `sizeof(Player)` print), so whether this version's demo allocations actually went through the pool branch or fell through to `malloc` (the `size <= 32` guard) depends on the actual compiled size.

#### The Solution & Refactoring:
* Replaced `delete[] rawMemory` with `std::free(rawMemory)` in the destructor, and `new char[96]` with `std::malloc(96)` in the constructor, to keep allocation/deallocation calls paired correctly (mixing `new[]`/`free` is undefined behavior) and to stop the pool's own teardown from re-triggering the global `delete` hook.

---

### v1.3 : Thread-Local Object Pool + Class-Specific Overload + Cache-Aligned Blocks

#### Design:
1. **True thread-local object instance:** `thread_local SimpleMemoryPool local_pool;` — a real per-thread object, not a pointer. Because it's a direct object instance, the compiler automatically invokes `~SimpleMemoryPool()` when each thread exits, giving a closed lifecycle with no manual cleanup step and no leaked arena.
2. **Class-specific overload:** Moved `operator new`/`operator delete` into `struct Player` itself (rather than global), so only `Player` allocations touch the pool — addressing the v1.2 program-wide interception risk.
3. **Block size increased to 64 bytes** (from 32), matching the hardware cache line width, fixing the block-overflow bug from v1.1 (`sizeof(Player)` was measured at 48 bytes, which fits inside a 64-byte block with 16 bytes to spare).
4. **224-byte arena with tail padding:** `std::malloc(64 * 3 + 32)` — three 64-byte slots plus a 32-byte tail safety margin at the end of the arena, guarding against out-of-bounds writes under aggressive compiler optimization (`-O3`).

#### Earlier draft superseded:
An earlier draft of this version used `thread_local SimpleMemoryPool* local_pool = nullptr;` (a pointer, lazily `new`'d on first use) and a 192-byte arena with no tail padding. That draft had a real bug: since nothing ever called `delete local_pool`, `~SimpleMemoryPool()` never ran on thread exit, silently leaking one arena per thread. The version described above — object instance, 224 bytes — is the one actually carried forward into the repo; the pointer-based draft was not.

#### Carried-over gap:
* `operator new` still doesn't handle pool exhaustion safely — `allocate()` can return `nullptr` when the free list is empty, but the C++ standard expects `operator new` to throw `std::bad_alloc` on failure (unless it's the `nothrow` overload). Returning `nullptr` silently means callers may dereference an invalid pointer. This was fixed in v1.4, where `allocate()` throws `std::bad_alloc` instead.

---

### v1.4 : Two-Stage Central Arena via OS Native Virtual Memory

#### Design:
1. **OS-native virtual space reservation:** A singleton `CentralArena` reserves a 400MB contiguous virtual address range at startup via `VirtualAlloc` (Windows) / `mmap` (POSIX), bypassing `malloc`/`new` for the top-level allocation.
2. **Two-stage slicing (the actual change from v1.3):** v1.3's `SimpleMemoryPool` got its 224-byte arena from `std::malloc`. In v1.4, each thread's `thread_local SimpleMemoryPool` instead requests a dedicated 64KB sub-chunk from the new `CentralArena` once, on first use — so the pool's raw memory now comes from a pre-reserved OS-native region instead of the general-purpose heap. The `thread_local SimpleMemoryPool local_pool;` object-instance pattern itself (and the automatic-destructor lifecycle it gives) was already established in v1.3 and is unchanged here.
3. **Locking scope, precisely stated:** `CentralArena::requestChunk()` is guarded by a `std::mutex`. This lock is only taken once per thread, during that thread's one-time 64KB chunk request (cold path / bootstrap). The actual `allocate()`/`deallocate()` hot-path loop used during normal game logic execution takes no locks. **Correction from earlier draft:** this pool is *not* "completely lock-free" — it is lock-free on the hot path with a mutex-protected one-time bootstrap per thread. The `std::atomic<size_t> offset` inside `CentralArena` does not by itself prevent cache-line contention or false sharing; the mutex, not the atomic, is what actually serializes the bootstrap-phase writes.
4. **Diagnostic logging** uses `std::this_thread::get_id()` to cross-reference kernel-scheduled threads against the addresses they receive.

#### Known code-level issues in this version:
* `size_t maxSlots = (myChunkSize - 64) / blockSize;` is commented as subtracting a "32-unit tail safety margin," but the code actually subtracts 64, not 32 — comment and code disagree; needs reconciling.
* `blockSize` is hardcoded to `64` in this version, not derived from `sizeof(T)`. The 64-byte alignment here is a fixed constant chosen in advance, not yet a computed property of the type being stored — that generalization is what v1.5 actually introduces. This version should not be described as doing "cache-aligned sizing" in the dynamic sense; it's a fixed, pre-chosen cache-line-sized block.
* `allocate()` now throws `std::bad_alloc()` on exhaustion (an improvement over v1.3's silent `nullptr`), but nothing in `game_core_worker` catches it — an exhausted pool would currently terminate the program via an uncaught exception rather than degrading gracefully.

---

### v1.5 : Template-Driven Generic Pool with Dynamic Workload Injection

#### Design:
1. **Generic, type-driven block sizing:** `SimpleMemoryPool<T>` now computes block size at compile time via `(sizeof(T) + 63) & ~63`, rounding up to the nearest 64-byte cache-line boundary based on the actual type stored. This is the version where cache-aligned sizing becomes a computed property rather than a hardcoded constant (see v1.4 note above).
2. **Placement-new architecture:** Removed `Player`'s custom `operator new`/`operator delete` overloads entirely. Worker code now explicitly does `::new (mem) Player{...}` and `p->~Player()`, decoupling raw memory recycling from object construction/destruction.
3. **Dynamic workload injection:** Replaced hardcoded per-core test data with `std::vector<UserRequest>` payloads dispatched from `main()`, so each core can process a variable number of requests.

#### Known limitations carried into this version (unchanged from v1.4 unless noted):
* `CentralArena::requestChunk()` still uses a mutex on the bootstrap path — same locking characterization as v1.4 applies here.
* Each `SimpleMemoryPool<T>` still requests exactly one fixed 64KB chunk at construction and has no mechanism to request additional chunks if it runs out — `allocate()` throws `std::bad_alloc` once exhausted, with no fallback growth path.
* `deallocate()` performs no sanity check that the returned address actually falls within `[rawMemory, rawMemory + chunkSize)` — a misdirected `deallocate()` call (e.g. wrong pool, double-free, foreign pointer) would silently corrupt the free list rather than being caught.
* Debug logging (`std::cout` + `cout_mtx` lock) is present directly in the `allocate()`/`deallocate()` hot path. This means current terminal output demonstrates *correctness*, not *performance* — the logging overhead currently dominates any timing measurement and has not yet been separated into a debug-only build path.
* No quantitative benchmark yet exists comparing this allocator against `std::malloc`/`new` or `tcmalloc`-style allocators under concurrent load — the "High-Performance" framing is currently a design-intent claim, not a measured one.

---

## Verification & Results

Confirmed via Linux terminal output (`g++ main.cpp -o main -pthread`):

1. **Thread-Local Storage Isolation:** Cores `[0]`, `[1]`, and `[2]` log entirely disjoint memory address ranges (e.g., `0x7f2e68000b60` vs `0x7f2e70000b60`), consistent with each thread receiving its own arena sub-chunk.
2. **Offset math:** The allocation delta between adjacent blocks within a pool (e.g., `0x7f2e68000ba0 - 0x7f2e68000b60`) is exactly `0x40` (64 bytes), consistent with the intended block size in v1.4/v1.5.
3. **Free list push-front recycling:** Returned addresses correctly become the new free-list head on the next `allocate()` call, consistent with O(1) LIFO reuse.
4. **Chunk request striding:** Address gaps between different threads' `CentralArena` sub-chunks were observed at `0x10000` (64KB) intervals, consistent with the fixed `myChunkSize` requested per thread.
5. **Object lifecycle:** Manual destructor invocation (`p->~Player()`) followed by `deallocate()` correctly releases the `std::string`'s internal heap buffer before the raw block returns to the free list, in the cases exercised by the current demo workloads.

**Not yet independently verified:**
* Actual absence of cache-line false sharing between adjacent thread chunks (asserted by design, not measured).
* Behavior under sustained allocate/deallocate pressure exceeding a single 64KB chunk per thread (no test currently exercises this, since it would currently throw `std::bad_alloc`).
* Performance relative to `std::malloc`/`new` — no benchmark has been run yet.

---

## Known Limitations

This project is under active revision. As of the current version (v1.5):

* **No dynamic arena growth:** each thread's pool is limited to a single fixed 64KB chunk; exhaustion throws rather than requesting more memory from `CentralArena`.
* **No deallocation safety checks:** `deallocate()` trusts the caller completely; there is no bounds check against the owning pool's memory range.
* **Hot-path logging:** `std::cout` calls inside `allocate()`/`deallocate()` are useful for demonstrating correctness but would need to be removed or gated behind a debug flag before any performance claim can be measured meaningfully.
* **No benchmark suite yet:** all "high-performance" framing is currently based on architectural reasoning (lock-free hot path, cache-line alignment, O(1) free-list operations), not measured throughput/latency numbers.
* **Locking is not fully eliminated:** `CentralArena::requestChunk()` uses a mutex during each thread's one-time bootstrap. This is intentionally scoped to the cold path, but should not be described as "completely lock-free" without that qualification.

## Next Steps
1. Add per-thread chunk growth (request additional 64KB chunks from `CentralArena` on exhaustion instead of throwing).
2. Add a debug/release build split to remove hot-path logging from performance-relevant runs.
3. Add a basic benchmark comparing this allocator against `std::malloc`/`new` under single- and multi-threaded load.
4. Add bounds-checked `deallocate()` (at minimum in debug builds) to catch misdirected frees early.
5. Consider replacing `CentralArena`'s bootstrap mutex with a lock-free `fetch_add`-based offset claim, if a true fully-lock-free arena is a design goal worth the added complexity.
