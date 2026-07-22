# High-Performance Memory Pool

* **Project:** High-Performance-Memory-Pool
* **Author:** HungYu
* **Date:** 2026-06-20
* **Status:** Work in progress / learning project — see [Known Limitations](#known-limitations) below.

---

## Project Overview

An exploration of how to implement a custom, O(1) complexity, thread-aware memory allocator in a multi-threaded C++ environment. The hot-path allocate/deallocate loop is lock-free **while a thread is still consuming blocks from its already-claimed chunk**; a mutex is still taken (a) once per thread on the one-time arena bootstrap, and (b) whenever a thread exhausts its current chunk and `grow_pool()` must claim a new one from `CentralArena`. See version notes below for exactly where locking does and doesn't apply.

---

## Build

This project uses a plain `Makefile` (no CMake required).

```bash
make            # Release: build `main` and `benchmark` (no logging, -O2)
make debug      # same targets, but defines POOL_DEBUG_LOG (prints [init]/[Pool] logs)
make clean      # remove built executables
./main          # run the demo (thread-local pool, prints player addresses)
./benchmark     # run the v1.7 benchmark (allocator vs system allocator)
```

* Requires a C++17 compiler and pthreads: `g++` (Linux) or MinGW-w64 / MSYS2 / WSL (Windows). On Linux `make` is available natively; on Windows you need MinGW-w64 / MSYS2 / WSL to provide both `make` and `g++`.
* The `Makefile` recipe lines must be indented with **Tab**, not spaces — watch this if you copy the file across editors.

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
5. **⚠️ Verified: `operator new` and `operator delete` use asymmetric logic, causing a real allocator/deallocator mismatch.** Testing on this codebase confirmed `sizeof(Player) = 48`, so `operator new`'s `size <= 32` guard correctly routes `Player` allocations to `malloc`, not the pool — the pool was never actually exercised by `Player` in this version's demo. However, `operator delete` has no equivalent size check:
   ```cpp
   void operator delete(void* address) noexcept{
       if(global_pool != nullptr && address != nullptr){
           global_pool -> deallocate(address);
           return;
       }
       free(address);
   }
   ```
   This means a `Player*` that was allocated via `malloc` (because it didn't fit the pool) still gets sent to `global_pool->deallocate()` on `delete`, inserting a `malloc`-owned 48-byte block into a free list built for 32-byte pool blocks. The pool has no way to know this memory doesn't actually belong to it. In this demo the program exits immediately afterward, so the mismatch doesn't visibly corrupt anything, but this is a real allocator/deallocator asymmetry — allocation is guarded by size, deallocation is not — that would cause real memory corruption if the pool were reused afterward (e.g., another `allocate()` call handing out this same address as if it were a valid 32-byte pool block).

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

### v1.6 : Debug-Gated Logging + Bounds-Checked Free + Chunk Reclamation + Dynamic Pool Growth

#### Design / Fixes (relative to v1.5):
1. **Hot-path logging gated behind `POOL_DEBUG_LOG`:** The `std::cout` + `cout_mtx` log blocks inside `allocate()` and `deallocate()` are now wrapped in `#ifdef POOL_DEBUG_LOG`, so a release build runs the hot path with zero logging overhead. The one-time `[init]` log emitted from `grow_pool()` is also gated behind `POOL_DEBUG_LOG`.
2. **Bounds-checked `deallocate()`:** `deallocate()` now walks `memory_list` and only accepts the address if (a) it falls inside one of the pool's owned chunks, (b) its offset from the chunk base is a multiple of `blockSize` (alignment check — rejects mid-block frees), and (c) that offset is `< myChunkSize - 64` (rejects the trailing 64-byte tail-padding region, which holds no valid block). A misdirected free that fails any of these is silently ignored (`return`) instead of corrupting the free list. Double-free of a *valid* block is still not detected (would require a separate in-use bitmap; deferred as out of scope for a learning project).
3. **Chunk reclamation — `CentralArena` offset is no longer monotonic:** `CentralArena` now keeps a `freeCentralHead` free list of returned chunks. `SimpleMemoryPool` owns a `std::vector<char*> memory_list` of every chunk it claimed, and its destructor calls `CentralArena::returnChunk(chunk)` for each — pushing the chunk back onto `freeCentralHead`. `requestChunk()` now prefers recycling a returned chunk from `freeCentralHead` before advancing `offset`. This fixes the v1.5 reclamation gap: a thread that exits returns its chunks to the arena for reuse, so `offset` no longer climbs unboundedly in a long-running server that spawns/joins threads.
4. **Dynamic pool growth — `grow_pool()`:** When `allocate()` finds an empty free list, it calls `grow_pool()`, which requests another 64KB chunk from `CentralArena`, appends it to `memory_list`, builds a fresh embedded free list inside it, and splices that new list's tail onto the previous free list (`current->next = temp_free_list`) — O(1) cross-chunk merge. The pool no longer throws `std::bad_alloc` on exhaustion; it grows instead (only throwing if `CentralArena` itself is out of space).

#### Code-level hardening in v1.6:
* **Overflow guard added to `grow_pool()`:** `blockSize = (sizeof(T) + 63) & ~63;` is computed *before* any use, and `if (blockSize > myChunkSize - 64) throw std::bad_alloc();` guards the subsequent `maxSlots = (myChunkSize - 64) / blockSize;` loop. This closes the v1.5 latent unsigned-underflow: with `blockSize > myChunkSize - 64` (e.g. a `T` larger than ~64KB), `maxSlots` would have been `0` and `maxSlots - 1` would underflow to `SIZE_MAX`, causing the build loop to scribble past the chunk. The guard is placed *after* `blockSize` is assigned, so the first (constructor) call is protected too.

#### Locking characterization in v1.6 (refines the "lock-free" claim):
* The `allocate()`/`deallocate()` hot path is **lock-free only while the calling thread is still consuming blocks from a chunk it already owns**. The moment a thread exhausts its current chunk and `grow_pool()` runs, it takes `arena_mtx` inside `CentralArena::requestChunk()`. So the correct statement is: *lock-free on the steady-state hot path; mutex-protected on the one-time bootstrap **and** on every chunk-growth event.*

#### Known limitations carried into this version (relative to v1.5):
* **Resolved:** No dynamic arena growth — now handled by `grow_pool()`.
* **Resolved:** No chunk reclamation — `CentralArena` now recycles returned chunks via `freeCentralHead`; `offset` is no longer monotonic.
* **Resolved (partially):** No deallocation safety checks — chunk-membership + alignment + tail-padding checks now applied; double-free of a valid block still undetected.
* **Resolved (partially):** Hot-path logging — now `POOL_DEBUG_LOG`-gated (both hot-path and `grow_pool` init logs); the debug/release split exists but is a single macro, not a separate build target.
* **Still open:** No benchmark suite yet — the "High-Performance" framing remains a design-intent claim, not measured.

---

### v1.7 : Build System + First Quantitative Benchmark

#### Design / Changes (relative to v1.6):
1. **Build system:** Added a plain `Makefile` that builds two targets — `main` (the demo) and `benchmark` (the new performance harness) — in one `make` invocation. `POOL_DEBUG_LOG` is no longer toggled by a hand-typed `-D` flag; instead `make debug` appends `-DPOOL_DEBUG_LOG` (plus `-Wall -Wextra`), while the default `make` builds a clean Release (`-O2`, no logging). This replaces the ad-hoc `g++ ... -DPOOL_DEBUG_LOG` workflow with a repeatable, single-command build.
2. **First benchmark (`benchmark.cpp`):** A standalone harness (self-contained copy of `CentralArena`/`SimpleMemoryPool`, no dependency on `main.cpp`) that measures allocation throughput against the system allocator. It runs two scenarios:
   * **Single-threaded** — pool vs `new`/`delete` for a fixed number of iterations. The iteration count deliberately exceeds one 64KB chunk (1023 slots), forcing `grow_pool()` to fire and thereby exercising the v1.6 dynamic-growth path under real execution.
   * **Multi-threaded** — one thread-local pool per hardware thread vs the system allocator, same allocate→construct→destroy→deallocate loop, reporting aggregate throughput.

#### Verified benchmark results (Linux, 6 hardware threads, Release build):

```
[A] single-threaded (forces cross-chunk growth)
  [single] pool            20000 iters   172.007 Mops/sec
  [single] system          20000 iters    61.241 Mops/sec

[B] multi-threaded (per-thread TLS pool vs system allocator)
  [multi ] pool        6 threads     300000 total   311.500 Mops/sec
  [multi ] system      6 threads     300000 total   254.013 Mops/sec
```

* **Single-threaded:** the pool is **~2.81× faster** than `new`/`delete`. The system allocator pays per-call lock + bookkeeping overhead on every allocation; the pool's hot path is an O(1), lock-free push/pop.
* **Multi-threaded:** the pool is **~1.23× faster** than the system allocator. Modern glibc `malloc` is itself per-thread (thread caches), so its multi-threaded contention is already low and the pool's advantage narrows — but the pool still wins.
* **Side effect:** the single-threaded run allocates 20000 `Player` objects (>1023 slots/chunk), so `grow_pool()` is guaranteed to have executed at least once during the run — confirming the v1.6 dynamic-growth path works at runtime, not just at compile time.

#### Honest scope of these numbers (do not over-read):
* This is a **microbenchmark in the allocator's most favorable regime**: fixed 48-byte objects, allocation-dominated tight loops, one type per pool. Real workloads (mixed sizes, large objects, non-allocation-bound logic) will show a smaller — possibly absent — advantage.
* `Mops/sec` includes `Player` construction + destruction (`std::string` bookkeeping); both sides pay that equally, so the delta is attributable to the allocator, but these are **not** pure allocator-only timings.
* Only throughput is measured. Latency distribution, false-sharing behavior, and long-running growth/reclaim churn have **not** been measured.

#### Known limitations carried into this version (relative to v1.6):
* **Resolved:** No benchmark suite — v1.7 adds `benchmark.cpp` with real measured numbers.
* **Resolved (partially):** Debug/release split — `make debug` vs `make` now controls `POOL_DEBUG_LOG`; still a single macro gate, not a CMake/IDE target, but the hand-typed `-D` is gone.
* **Still open:** double-free detection (deferred); `fetch_add` fully-lock-free arena (deferred); latency/false-sharing/churn instrumentation (deferred).

---

### v1.8 : Double-Free Detection via a Flattened In-Use Bitmap

#### Design / Changes (relative to v1.7):
1. **Per-block in-use tracking:** Added `std::vector<bool> in_use` as a member of `SimpleMemoryPool<T>`. Unlike `freeListHead`'s embedded-pointer trick, this state cannot live inside the block itself — a block that's currently on loan holds live caller data (e.g. a constructed `Player`), so there's no spare space inside it to also store a flag without corrupting that data. `in_use` is therefore a separate, out-of-band vector.
2. **Flattened indexing across chunks:** A pool may own multiple chunks (via `grow_pool()`, v1.6). `in_use` is laid out as one contiguous vector spanning all of them: chunk 0's `maxSlots` blocks occupy indices `[0, maxSlots)`, chunk 1's occupy `[maxSlots, 2*maxSlots)`, and so on. A block's flat index is computed as `chunk_number * maxSlots + local_index`, where `local_index = off / blockSize` (the same per-chunk offset math already used by v1.6's alignment check) and `chunk_number` is the position of the owning chunk within `memory_list`.
3. **`maxSlots` promoted to a member:** Previously a `grow_pool()`-local variable (v1.6/v1.7), `maxSlots` is now a class member, since `allocate()` and `deallocate()` both need it to convert a per-chunk local index into `in_use`'s flat index. This is safe because `myChunkSize` and `blockSize` are fixed for the pool's lifetime, so every chunk the pool owns has an identical `maxSlots`.
4. **`grow_pool()` extends `in_use` on every growth event:** After computing `maxSlots` for a newly claimed chunk, `grow_pool()` appends `maxSlots` fresh `false` entries to `in_use` — one per block in the new chunk, all initially free. This keeps `in_use`'s size in lockstep with however many chunks currently exist, rather than pre-sizing for a chunk count decided up front.
5. **`allocate()` marks a block in-use:** After popping a block off `freeListHead`, `allocate()` walks `memory_list` (same membership test as `deallocate()`'s bounds check) to find which chunk the popped block belongs to, computes its flat `in_use` index, and sets it to `true`.
6. **`deallocate()` rejects a double-free:** After the existing v1.6 bounds/alignment checks pass, `deallocate()` computes the flat index and checks `in_use[index]`. If it's already `false` — meaning this block isn't currently on loan to anyone — the call is a double-free (or a foreign pointer that coincidentally passed the earlier checks) and is silently rejected (`return`) without touching `freeListHead`. Only if the check passes does the function splice the block onto `freeListHead` and then set `in_use[index] = false`; the check must run *before* the splice, and the flag must be cleared *after* it, or the check would just be checking a flag it had already zeroed itself.

#### Why this matters:
Without this check, freeing the same valid address twice inserts it into `freeListHead` twice. The free list would then contain a self-referencing cycle, and a subsequent `allocate()` could hand out the *same* address to two different callers simultaneously — two live objects silently aliasing one block, each unaware the other exists, corrupting each other's state the moment either one writes to it.

#### Verified (hand-tested):
* **Alignment check (carried over from v1.7, re-verified in v1.8):** `deallocate()` called with a legitimately-borrowed address offset by +1 byte is rejected — confirmed by borrowing again immediately afterward and observing the next `allocate()` returns the *next* free-list block (base + `blockSize`), not the poisoned +1 address, proving the misaligned address was never spliced into `freeListHead`.
* **Overflow guard (carried over from v1.7, re-verified in v1.8):** Constructing `SimpleMemoryPool<T>` for a `T` whose `sizeof(T)` exceeds `myChunkSize - 64` (tested with a 100,000-byte struct) throws `std::bad_alloc` from inside `grow_pool()`, before `maxSlots` is ever computed — confirming the guard prevents the `size_t` underflow that would otherwise drive the block-splitting loop past the chunk's bounds.
* **Double-free detection:** Allocated one block (`p`), freed it once (legal), then freed it a second time (double-free). Then allocated twice more (`first`, `second`) and compared addresses:
  * `first == p` — expected regardless of the fix, since the first (legal) free correctly returned `p` to `freeListHead`.
  * `second == p + blockSize` (the normal next free-list entry), **not** `second == p`. Had the double-free not been caught, `p` would have been spliced into `freeListHead` twice, and `second` would have come back equal to `p` again instead of advancing to the next block. The observed result confirms the second `deallocate(p)` call was silently rejected and never touched the free list.

#### Known limitations carried into this version (relative to v1.7):
* **Resolved:** No double-free detection — `in_use` now catches a double-free of any block still tracked by this pool.
* **Still open, by design:** `in_use` only detects a double-free *of a block this pool actually owns and that passes the v1.6 bounds/alignment checks*. A foreign pointer that fails membership or alignment is already rejected by those earlier checks (as before v1.8) and never reaches the `in_use` lookup — this is existing, unchanged behavior, not a new gap.
* **Not addressed:** `CentralArena::requestChunk()`/`returnChunk()` still take `arena_mtx` on bootstrap and every chunk-growth event (unchanged from v1.6/v1.7). A proposal to replace this with a lock-free `fetch_add`/CAS-stack design was considered for this version and deliberately deferred — the naive version of that change (swapping the mutex for `compare_exchange_weak` on a raw-pointer stack) is vulnerable to the ABA problem and would need hazard pointers or a tagged/versioned pointer scheme to be done safely. That's a substantially harder, separately-scoped piece of work and is being left for a future version rather than rushed in alongside v1.8.
* **`in_use` adds a small per-block memory cost:** `std::vector<bool>` is bit-packed (1 bit per block, not 1 byte), so the overhead is small relative to `blockSize` (64 bytes), but it is a departure from the pool's original "zero metadata" design philosophy (v1.0–v1.5 stored no per-block state at all, only the embedded free-list pointer). This is a deliberate, documented trade-off: some out-of-band state is unavoidable once double-free detection is a goal, since (as established above) there's no spare room inside a live block to store it in-band.

## Verification & Results

Confirmed via Linux terminal output (`g++ main.cpp -o main -pthread`):

1. **Thread-Local Storage Isolation:** Cores `[0]`, `[1]`, and `[2]` log entirely disjoint memory address ranges (e.g., `0x7f2e68000b60` vs `0x7f2e70000b60`), consistent with each thread receiving its own arena sub-chunk.
2. **Offset math:** The allocation delta between adjacent blocks within a pool is exactly `0x40` (64 bytes), consistent with the intended block size in v1.4/v1.5/v1.6. *Stated for the current build where `sizeof(Player) = 48` (libstdc++ 64-bit) so `blockSize = 64`; if a platform's `std::string` layout pushed `sizeof(Player)` to `>64`, `blockSize` would round up to `128` and the delta would be `0x80`.*
3. **Free list push-front recycling:** Returned addresses correctly become the new free-list head on the next `allocate()` call, consistent with O(1) LIFO reuse.
4. **Chunk request striding:** Address gaps between different threads' `CentralArena` sub-chunks were observed at `0x10000` (64KB) intervals, consistent with the fixed `myChunkSize` requested per thread.
5. **Object lifecycle:** Manual destructor invocation (`p->~Player()`) followed by `deallocate()` correctly releases the `std::string`'s internal heap buffer before the raw block returns to the free list, in the cases exercised by the current demo workloads.
6. **Dynamic growth (v1.6):** Under a workload exceeding 1023 `Player` objects on a single core, `grow_pool()` claims a second 64KB chunk from `CentralArena` instead of throwing, and the new chunk's free list is spliced onto the old one; `SimpleMemoryPool`'s destructor returns every owned chunk to `CentralArena`'s `freeCentralHead` for reuse.
7. **Bounds-checked free (v1.6):** A `deallocate()` call with an address in the trailing 64-byte tail padding, or at a non-`blockSize`-aligned offset, is rejected (not inserted into the free list), observable when compiled with `-DPOOL_DEBUG_LOG` / `make debug`.
8. **Benchmark (v1.7):** See the v1.7 section above — pool beats the system allocator ~2.81× single-threaded and ~1.23× multi-threaded on this machine; the single-threaded run forces `grow_pool()` to execute at runtime.
9. **Double-free detection (v1.8):** A `deallocate()` call on a valid, block-aligned address that has *already* been freed once is rejected on the second call. Verified by allocating one block (`p`), freeing it twice, then allocating twice more: the first subsequent allocation correctly returns `p` (expected — the first free was legal), but the second returns the *next* free-list block (`p + blockSize`) rather than `p` itself — confirming the second `deallocate(p)` never re-spliced `p` into `freeListHead`. If the check had not fired, both subsequent allocations would have returned `p`.

**Not yet independently verified:**
* Actual absence of cache-line false sharing between adjacent thread chunks (asserted by design, not measured).
* Latency distribution and long-running growth/reclaim churn — no instrumentation yet.
* Performance on a workload with mixed object sizes or large objects (only the fixed 48-byte `Player` case is measured).

---

#### Benchmark addendum: v1.8 vs v1.7, measured with interleaved testing

After committing v1.8, `benchmark.cpp` was updated to include the new `in_use`/double-free-detection logic (previously only `main.cpp` had it, so the benchmark was silently still measuring v1.6-era pool behavior). This section documents both the corrected benchmark and a methodology mistake made — and fixed — while producing these numbers, on the theory that an honest record of a measurement error is more useful than a clean-looking number nobody can trust.

**What went wrong the first time:** The first attempt ran all v1.7 trials back-to-back, then all v1.8 trials back-to-back, and compared the two batches directly. A single run under this scheme showed `pool` apparently ~2× faster in v1.8 — but the *unmodified* `system` allocator baseline (`new`/`delete`, identical code in both binaries) also shifted by as much as 10× between batches. Since `system`'s code never changed, that shift can only be attributed to the environment itself (a shared VM) drifting between the two batches, not to anything in the pool's code. Comparing batch totals under these conditions would have attributed environmental noise to the code change.

**Fix — interleaved testing:** Both binaries were run in strict alternation (v1.7, v1.8, v1.7, v1.8, ...) via a small script (`interleaved_bench.sh`) so any drift in the VM's load is spread evenly across both versions instead of concentrated in whichever batch happened to run during a busy or idle period. Within each round, `pool ÷ system` was computed *before* aggregating — this ratio largely cancels out the drift that affects both binaries in that same round, whereas comparing raw pool numbers across rounds does not.

**Results (8 interleaved rounds, this machine — see environment note below):**

| Scenario | v1.7 median (pool ÷ system) | v1.8 median (pool ÷ system) |
|---|---|---|
| Single-threaded | 0.69 (pool ~31% slower than system) | 1.00 (roughly on par with system) |
| Multi-threaded (4 threads) | 0.80 (pool ~20% slower than system) | 1.09 (pool ~9% faster than system) |

The direction of improvement is consistent (single-threaded: 6/8 rounds favor v1.8; multi-threaded: 7/8 rounds favor v1.8), which is what makes this a usable result despite per-round absolute numbers varying widely (a symptom of running on a shared, noisy VM rather than dedicated hardware). The honest summary: **v1.8's pool closes the gap against the system allocator relative to v1.7, moving from consistently behind to roughly on par or slightly ahead** — not a clean multiplier, and not the dramatic "2×" a naive batch-vs-batch comparison suggested.

**Environment note — this is not the same machine as the v1.7 section above.** The original v1.7 numbers (`~2.81×` single-threaded, `~1.23×` multi-threaded) were measured on a dedicated 6-hardware-thread Linux machine. The v1.8 interleaved numbers above were measured on a different, 4-hardware-thread shared VM (the original machine was temporarily unreachable). The two environments are **not directly comparable** — this table shows v1.7-vs-v1.8 measured together on the same (noisier) machine, which is a valid same-environment comparison, but the absolute Mops/sec figures and multipliers here should not be placed side-by-side with the original v1.7 section's numbers as if they came from the same hardware. A same-hardware, same-methodology re-run on the original 6-thread machine remains a good future addition, but is not required to trust the relative v1.7-vs-v1.8 conclusion above, since that conclusion only depends on both versions having been measured under identical conditions as each other.

**Why single-threaded improved more than multi-threaded:** Plausible explanation, not confirmed — `in_use`'s bookkeeping in `allocate()`/`deallocate()` adds a fixed amount of per-call work (walking `memory_list`, computing a flat index) that doesn't scale with thread count. In the multi-threaded case, `CentralArena`'s `arena_mtx` contention and general 4-thread scheduling variance are already a larger share of total time, so a fixed per-call overhead added by `in_use` is proportionally smaller there than in the tighter single-threaded loop. This has not been independently verified (e.g. via profiling) and should be read as a hypothesis, not a finding.

---

## Known Limitations

This project is under active revision. As of the current version (v1.8):

* **Dynamic arena growth:** RESOLVED (v1.6) — `grow_pool()` claims additional 64KB chunks on exhaustion.
* **Chunk reclamation:** RESOLVED (v1.6) — `CentralArena` recycles returned chunks; `offset` is no longer monotonic.
* **Deallocation safety checks:** RESOLVED (v1.6 + v1.8) — membership, alignment, and tail-padding checked since v1.6; double-free of a valid block now also checked since v1.8 via the flattened `in_use` bitmap.
* **Hot-path logging / debug split:** RESOLVED (v1.7) — `make debug` toggles `POOL_DEBUG_LOG`; default `make` is silent Release.
* **Benchmark:** RESOLVED (v1.7) — `benchmark.cpp` measures vs the system allocator with real numbers (single ~2.81×, multi ~1.23× on this 6-thread machine). Scope is microbenchmark; see v1.7 honest-scope notes. **Not yet re-run against v1.8** — the `in_use` bookkeeping added to the `allocate()`/`deallocate()` hot path has not been benchmarked for overhead (see Next Steps #7).
* **Locking not fully eliminated:** `CentralArena::requestChunk()` takes `arena_mtx` on (a) each thread's one-time bootstrap and (b) every chunk-growth event. Steady-state consume path is lock-free, but growth is not. A `fetch_add`/CAS-based lock-free redesign was considered for v1.8 and deliberately deferred — see the v1.8 section above for why (ABA problem).
* **No double-free detection yet:** RESOLVED (v1.8) — see the v1.8 section above. Note the scope: `in_use` only catches a double-free of a block that already passes the v1.6 membership/alignment checks; a foreign or misaligned pointer is rejected earlier, as before.
* **No latency / false-sharing / churn instrumentation yet:** throughput-only microbenchmark so far.
* **`in_use` departs from the pool's original zero-metadata design:** v1.0–v1.5 stored no per-block state at all. v1.8 introduces one bit of out-of-band state per block to make double-free detection possible — a deliberate, documented trade-off, not an oversight.

## Next Steps
1. ~~Add per-thread chunk growth~~ — DONE in v1.6.
2. ~~Add a debug/release build split~~ — DONE in v1.7 (`make debug` vs `make`).
3. ~~Add a basic benchmark vs std::malloc/new~~ — DONE in v1.7 (`benchmark.cpp`), single ~2.81× / multi ~1.23× on a 6-thread Linux box.
4. ~~Add bounds-checked `deallocate()` double-free detection~~ — DONE in v1.8 (flattened `in_use` bitmap; membership + alignment + tail-padding + double-free all now checked).
5. Consider replacing `CentralArena`'s bootstrap/growth mutex with a lock-free `fetch_add`-based offset claim, if a true fully-lock-free arena (including growth) is a design goal worth the added complexity. **Note (v1.8):** a naive `compare_exchange_weak`-based free-chunk stack is vulnerable to ABA; a real implementation needs hazard pointers or tagged pointers, not just a mutex-to-CAS swap.
6. Extend the benchmark beyond the fixed-48-byte regime: mixed sizes, large objects, latency distribution, and sustained growth/reclaim churn.
7. Re-benchmark v1.8 against v1.7 to quantify the cost (if any) of the `in_use` bookkeeping added to the `allocate()`/`deallocate()` hot path.