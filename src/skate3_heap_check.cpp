// Integrity checker for the guest's small-block pool allocator.
//
// The remaining map-load crash is heap corruption, not a null pointer. A
// --protect_zero=true run caught a WRITE to guest 0x00000004 on render_thread
// with lr = 0x82990940 (sub_82990768 +0x1D8). Decoded:
//
//   sub_82990768 (pool alloc)  read the current chunk's free-block list head
//                              [chunk+8] and got 0xFFFFFFFF
//                              -> treated it as a free block, rounded it down
//                                 to its 32 KiB chunk header:
//                                 ((0xFFFFFFFF+0x7FFF) & 0xFFFF8000) - 0x8000
//                                 = 0xFFFF8000
//   sub_8298CEF8 (list unlink) node->next and node->prev at 0xFFFF8000 read as
//                              0, so "next->prev = prev" stored through null.
//
// The null store is three steps downstream of the defect. The defect is that
// 0xFFFFFFFF was in the free list at all - something wrote -1 into a block that
// was sitting free, or into the chunk header itself. By the time the allocator
// trips over it the writer is long gone, so this file catches the corruption at
// the earliest moment it is observable and records enough to name the writer:
//
//   1. Every alloc sweeps all 20 size-class bins and checks each bin's head
//      chunk's free-list head. A legitimate value is 0 or a block inside that
//      chunk; anything else is definitive, no lock needed. This fires one step
//      BEFORE the crash, while the chunk pointer is still in hand.
//   2. Every free poisons the block it is about to release (past word 0, which
//      the allocator uses for the free-list link) and records who freed it.
//      Every alloc verifies the poison of the block it hands back. A block that
//      changed while it was free is a use-after-free, and the recorded free-site
//      lr names the subsystem that owned it.
//
// Off by default: it costs a 40-load sweep plus a block-sized fill per pool op.
//
// Layout below was decoded from generated/skate3_recomp.53.cpp (sub_82990768,
// sub_82990A58, sub_8298CEF8) - do not re-derive it.
//
//   pool  +8            critical section (the allocator's own lock)
//         +56/+60       free-chunk count / free-chunk list
//         +64 + bin*8   bin chunk list {head, tail}   bins 0..19
//         +224/+228     arena [lo, hi)  (the guest's own "is this mine" test)
//         +232 + bin*4  per-bin counters
//         +312/+316     live chunk count / high-water
//
//   chunk +0/+4         next/prev  (+4 is stamped 0xDEC6FBAD when the whole
//                       chunk is released)
//         +8            free-block list head   <- the word that was corrupt
//         +12/+14/+16   u16 block size / blocks per chunk / blocks in use
//         +20           first block (= chunk+32)
//         +32...        blocks; a free block holds its "next" in word 0 only
//
// Chunks are 32 KiB aligned and a block's chunk is
// ((block + 32767) & 0xFFFF8000) - 32768.

#include "generated/skate3_init.h"

#include <execinfo.h>
#include <signal.h>
#include <sys/prctl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

REXCVAR_DEFINE_BOOL(skate3_heap_check, false, "Skate 3",
                    "Validate the guest small-block pool on every alloc/free "
                    "and report the first corrupt free list")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_heap_check_poison, true, "Skate 3",
                    "Also poison freed pool blocks and verify them on reuse "
                    "(catches use-after-free writes; costs a fill per free)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_heap_check_abort, true, "Skate 3",
                    "Abort on the first heap-check failure so the crash "
                    "reporter dumps the guest state at the point of detection")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(skate3_instance_free_defer_ms, 0, "Skate 3",
                     "CANDIDATE FIX for the cross-thread use-after-free: hold "
                     "the u16 arrays freed by the WorldPresentation instance "
                     "teardown for this many ms, so a Job Manager worker still "
                     "walking an interior pointer into them writes to memory "
                     "nobody has reused (0 = off)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_heap_quarantine_kb, 0, "Skate 3",
                     "Hold this many KB of freed pool blocks back from the "
                     "allocator, so a stale write has a still-free block to "
                     "land on for the watch to catch (0 = off)")
    .range(0, 2048)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_heap_watch_selftest, false, "Skate 3",
                    "Prove the store watch works: mark one freshly allocated "
                    "block as free and expect its initialisation to trip it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_heap_watch, false, "Skate 3",
                    "Trap the guest store that writes into a freed pool block. "
                    "Needs the REX_STORE_* watch hook in skate3_init.h")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Store watch, armed over the whole pool arena with a shadow bit per 16-byte
// granule saying "this granule is inside a block that is currently FREE".
// Every guest store checks it (see REX_WATCH_CHECK in generated/skate3_init.h),
// so the writer is caught in the act instead of being inferred from a poison
// diff hundreds of thousands of pool ops later.
//
// Nothing legitimate writes to a free block: the allocator only writes word 0
// when it PUSHES a block (before we mark it) and when it re-carves a released
// chunk (we clear the whole chunk's bits then). Any hit is the bug.
extern "C" {
uint32_t g_skate3_watch_lo = 0;
uint32_t g_skate3_watch_len = 0;  // 0 disarms the check
uint8_t* g_skate3_watch_bits = nullptr;
}

thread_local bool tl_in_instance_dtor = false;

namespace {

constexpr uint32_t kBins = 20;
constexpr uint32_t kBinList = 64;   // pool + kBinList + bin*8 = {head, tail}
constexpr uint32_t kArenaLo = 224;
constexpr uint32_t kArenaHi = 228;

constexpr uint32_t kChunkSize = 32768;
constexpr uint32_t kChunkData = 32;  // first block offset within a chunk
constexpr uint32_t kChunkFreeHead = 8;
constexpr uint32_t kChunkBlockSize = 12;  // u16
constexpr uint32_t kChunkCapacity = 14;   // u16
constexpr uint32_t kChunkUsed = 16;       // u16

// Freed-block fill. Not a valid guest pointer, so anything that follows a
// poisoned link faults loudly instead of wandering.
constexpr uint8_t kPoison = 0xCD;

constexpr int kFreeFrames = 12;

struct FreeRecord {
  uint32_t lr = 0;          // guest caller that freed the block
  uint32_t block_size = 0;  // size class at the time, to spot recycled chunks
  uint64_t seq = 0;         // pool-op counter, for "how long ago"
  uint64_t epoch = 0;       // chunk generation, see g_epoch
  char thread[16] = {0};
  // Host frames of the free. The generated code compiles one host function per
  // guest function, so these ARE the guest call chain - and unlike lr (which
  // only ever names the allocator's own wrapper) they reach the game code that
  // owned the block.
  void* frames[kFreeFrames] = {nullptr};
  int frame_count = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, FreeRecord> g_freed;
// A chunk whose last block is released goes back on the pool's free-chunk list
// and can be re-carved at a different size class, so every block address in it
// may become a different object - or the interior of one. Bumping a generation
// counter there and matching it on verify is what keeps that churn from being
// misread as a use-after-free.
std::unordered_map<uint32_t, uint64_t> g_epoch;
std::atomic<uint64_t> g_seq{0};
std::atomic<uint64_t> g_allocs{0};
std::atomic<uint64_t> g_frees{0};
std::atomic<int> g_reports{0};
std::atomic<int> g_modulus_warns{0};

struct QuarantinedBlock {
  uint32_t pool;
  uint32_t block;
  uint32_t size;
};
std::mutex g_quarantine_mutex;
std::deque<QuarantinedBlock> g_quarantine;
uint64_t g_quarantine_bytes = 0;

uint64_t QuarantineBytes() {
  static const uint64_t bytes =
      uint64_t(REXCVAR_GET(skate3_heap_quarantine_kb)) * 1024;
  return bytes;
}

// Blocks freed by Sk8::WorldPresentation's instance teardown (sub_82792040)
// are the ones that matter: it frees two u16 arrays whose INTERIOR pointers
// (&A[idx]) sub_82791CF8 published into other objects, so a stale holder
// writing its u16 "invalid" markers lands exactly here. Giving them their own
// quarantine means the budget is not spent on unrelated pool traffic, and they
// stay unreusable for minutes rather than a second.

std::deque<QuarantinedBlock> g_dtor_quarantine;
uint64_t g_dtor_quarantine_bytes = 0;
std::atomic<uint64_t> g_dtor_blocks{0};


bool Enabled() {
  static const bool on = REXCVAR_GET(skate3_heap_check);
  return on;
}

uint64_t DeferMs() {
  static const uint64_t ms = uint64_t(REXCVAR_GET(skate3_instance_free_defer_ms));
  return ms;
}

// The fix must be usable on its own: the diagnostic half (poison, watch, bin
// sweep) costs far too much to leave on in normal play.
bool HooksActive() { return Enabled() || DeferMs() != 0; }

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct DeferredBlock {
  uint32_t pool;
  uint32_t block;
  uint32_t size;
  uint64_t due_ms;
};
std::mutex g_defer_mutex;
std::deque<DeferredBlock> g_defer;
uint64_t g_defer_bytes = 0;
// Safety valve: never hold more than this, however short the deferral turns out
// to be relative to job latency. A starved pool is a worse failure than the bug.
constexpr uint64_t kDeferMaxBytes = 512 * 1024;

bool PoisonEnabled() {
  static const bool on = REXCVAR_GET(skate3_heap_check_poison);
  return on;
}

bool AbortOnFailure() {
  static const bool on = REXCVAR_GET(skate3_heap_check_abort);
  return on;
}

uint32_t ChunkOf(uint32_t block) {
  // Exactly the guest's arithmetic, wraparound included.
  return ((block + 32767u) & 0xFFFF8000u) - 32768u;
}

const char* ThreadName() {
  static thread_local char name[20] = {0};
  if (name[0] == 0 &&
      prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0) != 0) {
    name[0] = '?';
    name[1] = 0;
  }
  return name;
}

// The pool object is static data that exists before it is initialised; treat an
// implausible arena as "not ready" rather than as a failure.
bool PoolArena(uint8_t* base, uint32_t pool, uint32_t* lo, uint32_t* hi) {
  if (pool < 0x10000) {
    return false;
  }
  *lo = REX_LOAD_U32(pool + kArenaLo);
  *hi = REX_LOAD_U32(pool + kArenaHi);
  return *lo >= 0x10000 && *hi > *lo;
}

void LogFrames(const char* what, void* const* frames, int n) {
  char** syms = ::backtrace_symbols(const_cast<void* const*>(frames), n);
  for (int i = 0; i < n; ++i) {
    REXLOG_ERROR("heap-check:   {} #{} {}", what, i,
                 syms != nullptr ? syms[i] : "<no symbol>");
  }
  ::free(syms);
}

void LogHostBacktrace() {
  void* frames[24];
  const int n = ::backtrace(frames, 24);
  LogFrames("now", frames, n);
}

// 64 bytes of the block around an offset, raw guest bytes. The shape of the
// write is the tell: one pointer-sized field says a stale link was updated, a
// contiguous run says a whole object was written through a dangling this.
void DumpBlock(uint8_t* base, uint32_t block, uint32_t size, uint32_t around) {
  const uint32_t start = (around >= 32 ? around - 32 : 0) & ~15u;
  const uint32_t end = std::min(start + 64, size);
  for (uint32_t off = start; off < end; off += 16) {
    const uint8_t* p = base + block + off;
    const uint32_t n = std::min(16u, end - off);
    char hex[64] = {0};
    for (uint32_t i = 0; i < n; ++i) {
      std::snprintf(hex + i * 3, 4, "%02X ", p[i]);
    }
    REXLOG_ERROR("heap-check:   +{:<4} {}", off, hex);
  }
}

void DumpChunk(uint8_t* base, uint32_t chunk) {
  REXLOG_ERROR(
      "heap-check:   chunk 0x{:08X}: next=0x{:08X} prev=0x{:08X} "
      "freehead=0x{:08X} bsize={} cap={} used={} first=0x{:08X}",
      chunk, REX_LOAD_U32(chunk + 0), REX_LOAD_U32(chunk + 4),
      REX_LOAD_U32(chunk + kChunkFreeHead), REX_LOAD_U16(chunk + kChunkBlockSize),
      REX_LOAD_U16(chunk + kChunkCapacity), REX_LOAD_U16(chunk + kChunkUsed),
      REX_LOAD_U32(chunk + 20));
}

// One failure is all we need; the state is freshest at the first one.
void Fail(uint8_t* base, uint32_t pool, uint32_t chunk, uint32_t guest_lr) {
  if (g_reports.fetch_add(1) != 0) {
    return;
  }
  REXLOG_ERROR(
      "heap-check:   pool=0x{:08X} arena=[0x{:08X},0x{:08X}) thread={} "
      "guest lr=0x{:08X} after {} allocs / {} frees",
      pool, REX_LOAD_U32(pool + kArenaLo), REX_LOAD_U32(pool + kArenaHi),
      ThreadName(), guest_lr, g_allocs.load(), g_frees.load());
  if (chunk != 0) {
    DumpChunk(base, chunk);
  }
  LogHostBacktrace();
  rex::FlushLogging();
  if (AbortOnFailure()) {
    // SIGABRT: skate3_crash_report turns it into a full guest register dump in
    // <log_file>.crash, at the point of DETECTION rather than four allocations
    // later at the fault.
    ::raise(SIGABRT);
  }
}

// A chunk's free-list head is either 0 or a block inside that chunk. Nothing
// else is legal, at any moment, with or without the pool lock held - which is
// what makes this safe to check from outside the guest's critical section.
//
// Returns false (and reports) when the value cannot be explained.
bool CheckChunkFreeHead(uint8_t* base, uint32_t pool, uint32_t chunk,
                        uint32_t guest_lr, const char* where) {
  const uint32_t head = REX_LOAD_U32(chunk + kChunkFreeHead);
  if (head == 0) {
    return true;
  }
  const uint32_t lo = chunk + kChunkData;
  const uint32_t hi = chunk + kChunkSize;
  if (head >= lo && head < hi && (head & 3u) == 0) {
    // Secondary signal only. The chunk's block size is rewritten a few stores
    // after the chunk is linked into its bin, so a mid-init read can see a size
    // from the chunk's previous life and fail the modulus while the pointer
    // itself is perfectly valid. Worth knowing about, never worth aborting on.
    const uint32_t bsize = REX_LOAD_U16(chunk + kChunkBlockSize);
    if (bsize != 0 && (head - lo) % bsize != 0 &&
        g_modulus_warns.fetch_add(1) < 4) {
      REXLOG_WARN(
          "heap-check: {} free head 0x{:08X} is not on a block boundary "
          "(chunk 0x{:08X}, block size {}) - benign if it is a mid-init race",
          where, head, chunk, bsize);
    }
    return true;
  }
  REXLOG_ERROR(
      "heap-check: CORRUPT free list at {}: chunk 0x{:08X} free head is "
      "0x{:08X}, which is outside [0x{:08X},0x{:08X}). This is the value the "
      "allocator would have handed out as a block.",
      where, chunk, head, lo, hi);
  Fail(base, pool, chunk, guest_lr);
  return false;
}

// The free-list link lives in word 0 of a free block, which is the one word the
// poison cannot cover (the allocator writes it). It is still checkable, one step
// removed: the value the allocator just popped out of a block IS the chunk's new
// free head, so a bad head right after an allocation names the block whose link
// word was clobbered - and that block's free record names who owned it.
void CheckPoppedLink(uint8_t* base, uint32_t pool, uint32_t chunk,
                     uint32_t block, uint32_t guest_lr) {
  const uint32_t head = REX_LOAD_U32(chunk + kChunkFreeHead);
  if (head == 0 || (head >= chunk + kChunkData && head < chunk + kChunkSize &&
                    (head & 3u) == 0)) {
    return;
  }
  REXLOG_ERROR(
      "heap-check: CORRUPT free-list LINK: block 0x{:08X} was handed out and "
      "its link word carried 0x{:08X} into the chunk's free head - the next "
      "allocation from chunk 0x{:08X} would have returned that as a block.",
      block, head, chunk);
  FreeRecord rec;
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_freed.find(block);
    if (it != g_freed.end()) {
      rec = it->second;
      have = true;
    }
  }
  if (have) {
    REXLOG_ERROR("heap-check:   that block was freed on thread {} here:",
                 rec.thread);
    LogFrames("freed-by", rec.frames, rec.frame_count);
  }
  Fail(base, pool, chunk, guest_lr);
}

// Sweep every bin's head chunk. That head is the chunk the next allocation of
// that size class will draw from, so this covers exactly the state the crash
// walked into.
void CheckAllBins(uint8_t* base, uint32_t pool, uint32_t guest_lr,
                  const char* where) {
  uint32_t lo = 0, hi = 0;
  if (!PoolArena(base, pool, &lo, &hi)) {
    return;
  }
  for (uint32_t bin = 0; bin < kBins; ++bin) {
    const uint32_t head = REX_LOAD_U32(pool + kBinList + bin * 8);
    if (head == 0) {
      continue;
    }
    if ((head & (kChunkSize - 1)) != 0 || head < lo || head >= hi) {
      REXLOG_ERROR(
          "heap-check: CORRUPT bin list at {}: bin {} head chunk is 0x{:08X} "
          "(not 32 KiB aligned, or outside the arena [0x{:08X},0x{:08X}))",
          where, bin, head, lo, hi);
      Fail(base, pool, 0, guest_lr);
      return;
    }
    if (!CheckChunkFreeHead(base, pool, head, guest_lr, where)) {
      return;
    }
  }
}

// Periodic proof that the hooks are actually running. Absence of a failure
// report is not evidence of a healthy heap; absence of THIS line means the run
// never reached the allocator and says nothing at all.
void Liveness(uint64_t allocs) {
  // The first one lands within seconds of boot and proves the override took;
  // the rest are frequent enough that a 300 s run prints several, which is how
  // the quarantines get verified as actually holding something.
  if (allocs != 1 && allocs % 250'000 != 0) {
    return;
  }
  size_t tracked = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    tracked = g_freed.size();
  }
  size_t q = 0, dq = 0;
  uint64_t qb = 0, dqb = 0;
  {
    std::lock_guard<std::mutex> lock(g_quarantine_mutex);
    q = g_quarantine.size();
    qb = g_quarantine_bytes;
    dq = g_dtor_quarantine.size();
    dqb = g_dtor_quarantine_bytes;
  }
  REXLOG_INFO(
      "heap-check: {} allocs / {} frees, {} tracked free; quarantine {} blocks "
      "({} KB), instance-teardown quarantine {} blocks ({} KB) from {} teardown "
      "frees",
      allocs, g_frees.load(std::memory_order_relaxed), tracked, q, qb / 1024, dq,
      dqb / 1024, g_dtor_blocks.load(std::memory_order_relaxed));
}

// --- store watch -----------------------------------------------------------

std::atomic<bool> g_selftest_armed{false};

bool WatchSelftest() {
  static const bool on = REXCVAR_GET(skate3_heap_watch_selftest);
  return on;
}

bool WatchEnabled() {
  static const bool on = REXCVAR_GET(skate3_heap_watch);
  return on;
}

std::atomic<bool> g_watch_armed{false};

// Both observed corruptions were in the pool at 0x830DACA8 (a static address in
// the guest image, identical across runs). Other pools exist, and the watch has
// one arena's worth of shadow bits, so prefer that one and say so - watching the
// wrong arena would waste a whole run and look exactly like "no findings".
constexpr uint32_t kPoolOfInterest = 0x830DACA8;

void WatchArm(uint32_t pool, uint32_t lo, uint32_t hi) {
  if (pool != kPoolOfInterest) {
    static std::atomic<int> other{0};
    if (other.fetch_add(1) == 0) {
      REXLOG_INFO(
          "heap-check: store watch skipping pool 0x{:08X} (waiting for the one "
          "at 0x{:08X})",
          pool, kPoolOfInterest);
    }
    return;
  }
  if (!WatchEnabled() || g_watch_armed.load(std::memory_order_relaxed)) {
    return;
  }
  const uint32_t len = hi - lo;
  if (len == 0 || len > 64u * 1024 * 1024) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, [lo, len] {
    g_skate3_watch_bits = static_cast<uint8_t*>(std::calloc(len / 128 + 1, 1));
    if (g_skate3_watch_bits == nullptr) {
      return;
    }
    g_skate3_watch_lo = lo;
    // Publish the length last: it is what arms the check in the store macro.
    __atomic_store_n(&g_skate3_watch_len, len, __ATOMIC_RELEASE);
    g_watch_armed.store(true, std::memory_order_relaxed);
    REXLOG_INFO(
        "heap-check: store watch armed over pool 0x{:08X} arena "
        "[0x{:08X},0x{:08X})",
        kPoolOfInterest, lo, lo + len);
  });
}

void WatchMark(uint32_t addr, uint32_t size, bool free_now) {
  if (g_skate3_watch_len == 0 || addr - g_skate3_watch_lo >= g_skate3_watch_len) {
    return;
  }
  const uint32_t base_off = addr - g_skate3_watch_lo;
  for (uint32_t off = 0; off < size; off += 16) {
    const uint32_t g = (base_off + off) >> 4;
    uint8_t& byte = g_skate3_watch_bits[g >> 3];
    const uint8_t bit = uint8_t(1u << (g & 7));
    if (free_now) {
      __atomic_or_fetch(&byte, bit, __ATOMIC_RELAXED);
    } else {
      __atomic_and_fetch(&byte, uint8_t(~bit), __ATOMIC_RELAXED);
    }
  }
}

uint8_t* g_base = nullptr;
std::atomic<uint32_t> g_pool{0};

// Replicates the guest's own ownership test in sub_82990A58, plus a block
// boundary check, so we never write poison into memory this pool does not own.
bool BlockExtent(uint8_t* base, uint32_t pool, uint32_t block, uint32_t* chunk,
                 uint32_t* size) {
  uint32_t lo = 0, hi = 0;
  if (!PoolArena(base, pool, &lo, &hi) || block < lo || block >= hi) {
    return false;
  }
  const uint32_t c = ChunkOf(block);
  if (block < c || block >= c + kChunkSize) {
    return false;
  }
  const uint32_t bsize = REX_LOAD_U16(c + kChunkBlockSize);
  if (bsize < 16 || bsize > 2048 || block < c + kChunkData ||
      (block - (c + kChunkData)) % bsize != 0 ||
      block + bsize > c + kChunkSize) {
    return false;
  }
  *chunk = c;
  *size = bsize;
  return true;
}

}  // namespace

// Called from REX_STORE_* (see generated/skate3_init.h) when a guest store
// lands in a granule marked free. noinline and non-static so the return address
// is the store site inside the recompiled function - which, resolved against
// the binary, IS the guest function that wrote it.
extern "C" __attribute__((noinline)) void skate3_heap_watch_hit(uint32_t ea,
                                                                uint32_t size) {
  static std::atomic<int> hits{0};
  if (hits.fetch_add(1) != 0) {
    return;
  }
  // One is all we need, and the report below does not stop the process (the
  // crash reporter declines SIGABRT). Disarm so the rest of the session does
  // not pay a call per store into the arena.
  __atomic_store_n(&g_skate3_watch_len, 0u, __ATOMIC_RELEASE);
  uint8_t* base = g_base;
  void* site = __builtin_return_address(0);
  REXLOG_ERROR(
      "heap-check: WRITE INTO A FREE BLOCK: guest 0x{:08X}, {} bytes, from "
      "thread {}. Nothing legitimate writes to a free block - this is the "
      "corrupting store.",
      ea, size, ThreadName());
  LogFrames("writer", &site, 1);
  LogHostBacktrace();
  if (base == nullptr) {
    return;
  }
  const uint32_t chunk = ChunkOf(ea);
  const uint32_t bsize = REX_LOAD_U16(chunk + kChunkBlockSize);
  if (bsize >= 16 && bsize <= 2048 && ea >= chunk + kChunkData) {
    const uint32_t block =
        chunk + kChunkData + (ea - (chunk + kChunkData)) / bsize * bsize;
    REXLOG_ERROR("heap-check:   that is +{} of free block 0x{:08X} (size {})",
                 ea - block, block, bsize);
    FreeRecord rec;
    bool have = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_freed.find(block);
      if (it != g_freed.end()) {
        rec = it->second;
        have = true;
      }
    }
    if (have) {
      REXLOG_ERROR("heap-check:   it was freed on thread {} here:", rec.thread);
      LogFrames("freed-by", rec.frames, rec.frame_count);
    }
  }
  Fail(base, g_pool.load(), chunk, 0);
}

// Pool alloc: r3 = pool, r4 = size, returns the block in r3 (0 on failure).
// Sizes over 2048 and the wrapper entry points (sub_8298D078/sub_8298D098 add 8
// to r3 and tail-call into here) all funnel through this one function.
extern "C" REX_FUNC(sub_82990768) {
  if (!HooksActive()) {
    __imp__sub_82990768(ctx, base);
    return;
  }
  const uint32_t pool = ctx.r3.u32;
  const uint32_t guest_lr = uint32_t(ctx.lr);
  Liveness(g_allocs.fetch_add(1, std::memory_order_relaxed) + 1);
  g_base = base;
  g_pool.store(pool, std::memory_order_relaxed);
  if (WatchEnabled() && !g_watch_armed.load(std::memory_order_relaxed)) {
    uint32_t alo = 0, ahi = 0;
    if (PoolArena(base, pool, &alo, &ahi)) {
      WatchArm(pool, alo, ahi);
    }
  }

  CheckAllBins(base, pool, guest_lr, "alloc entry");

  __imp__sub_82990768(ctx, base);

  const uint32_t block = ctx.r3.u32;
  if (block == 0) {
    return;
  }

  // The block is ours alone between the allocator returning it and the caller
  // touching it, so this needs no lock against the guest.
  uint32_t chunk = 0, bsize = 0;
  if (!BlockExtent(base, pool, block, &chunk, &bsize)) {
    return;
  }
  CheckPoppedLink(base, pool, chunk, block, guest_lr);
  // The block must be inside the WATCHED arena - the game runs more than one
  // pool, and arming on a block from the other one marks nothing and proves
  // nothing.
  if (WatchSelftest() && g_skate3_watch_len != 0 &&
      block - g_skate3_watch_lo < g_skate3_watch_len &&
      !g_selftest_armed.exchange(true)) {
    // Deliberately leave a block that was just handed out marked FREE. Its new
    // owner is about to initialise it, and that store must trip the watch. No
    // hit here means the watch is broken, and every quiet run that follows
    // would be quiet for the wrong reason.
    WatchMark(block, bsize, /*free_now=*/true);
    REXLOG_INFO(
        "heap-check: SELFTEST armed on live block 0x{:08X} (size {}) - the next "
        "guest store into it must trip the watch",
        block, bsize);
  } else {
    WatchMark(block, bsize, /*free_now=*/false);
  }
  if (!PoisonEnabled()) {
    return;
  }
  FreeRecord rec;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_freed.find(block);
    if (it == g_freed.end()) {
      return;  // never passed through our free hook - nothing to verify
    }
    rec = it->second;
    g_freed.erase(it);
    auto ep = g_epoch.find(chunk);
    const uint64_t epoch = ep == g_epoch.end() ? 0 : ep->second;
    if (rec.epoch != epoch || rec.block_size != bsize) {
      return;  // chunk was released and re-carved; the poison means nothing now
    }
  }

  const uint8_t* bytes = base + block;
  uint32_t first = 0, last = 0, changed = 0;
  for (uint32_t off = 4; off < bsize; ++off) {
    if (bytes[off] == kPoison) {
      continue;
    }
    if (changed++ == 0) {
      first = off;
    }
    last = off;
  }
  if (changed == 0) {
    return;
  }
  REXLOG_ERROR(
      "heap-check: USE AFTER FREE: block 0x{:08X} (size {}) was written while "
      "it sat on the free list - {} bytes changed in +{}..+{}, word at +{} is "
      "0x{:08X}. Freed on thread {} (guest lr=0x{:08X}), {} pool ops ago.",
      block, bsize, changed, first, last, first & ~3u,
      REX_LOAD_U32(block + (first & ~3u)), rec.thread, rec.lr,
      g_seq.load() - rec.seq);
  DumpBlock(base, block, bsize, first);
  LogFrames("freed-by", rec.frames, rec.frame_count);
  Fail(base, pool, chunk, guest_lr);
}

// Pool free: r3 = pool, r4 = ptr, returns 1 if this pool owned the pointer.
extern "C" REX_FUNC(sub_82990A58) {
  if (!HooksActive()) {
    __imp__sub_82990A58(ctx, base);
    return;
  }
  const uint32_t pool = ctx.r3.u32;
  const uint32_t block = ctx.r4.u32;
  const uint32_t guest_lr = uint32_t(ctx.lr);
  g_frees.fetch_add(1, std::memory_order_relaxed);

  uint32_t chunk = 0, bsize = 0;
  const bool ours = BlockExtent(base, pool, block, &chunk, &bsize);

  // --- candidate fix: defer the instance-teardown arrays ------------------
  // Only these blocks, and only for a bounded time. A job that captured an
  // interior pointer into them keeps writing after teardown; holding the
  // memory means those writes land somewhere nobody has reused, instead of on
  // a free-list link word.
  if (ours && tl_in_instance_dtor && DeferMs() != 0) {
    std::vector<DeferredBlock> release;
    const uint64_t now = NowMs();
    if (Enabled()) {
      // Keep the block visible to the watch while it is held. That is what
      // makes the fix falsifiable: if it is working, the job's write still
      // trips the watch (it really does write after the free) but no
      // CORRUPT free-list LINK ever appears, because the write landed on
      // memory the allocator does not own. "Nothing happened" would not
      // distinguish a working fix from a quiet run.
      std::memset(base + block, kPoison, bsize);
      WatchMark(block, bsize, /*free_now=*/true);
    }
    {
      std::lock_guard<std::mutex> lock(g_defer_mutex);
      g_defer.push_back({pool, block, bsize, now + DeferMs()});
      g_defer_bytes += bsize;
      while (!g_defer.empty() &&
             (g_defer.front().due_ms <= now || g_defer_bytes > kDeferMaxBytes)) {
        release.push_back(g_defer.front());
        g_defer_bytes -= g_defer.front().size;
        g_defer.pop_front();
      }
    }
    for (const DeferredBlock& d : release) {
      if (Enabled()) {
        WatchMark(d.block, d.size, /*free_now=*/false);
      }
      ctx.r3.u32 = d.pool;
      ctx.r4.u32 = d.block;
      __imp__sub_82990A58(ctx, base);
      // Same bookkeeping the normal path does. Skipping it leaves the watch
      // bits set on a chunk that later empties and gets re-carved, and the
      // allocator's own legitimate write to each block's link word then trips
      // the watch - a false positive that reads exactly like the real bug.
      if (Enabled()) {
        WatchMark(d.block, d.size, /*free_now=*/true);
        const uint32_t dchunk = ChunkOf(d.block);
        if (REX_LOAD_U16(dchunk + kChunkUsed) == 0) {
          WatchMark(dchunk, kChunkSize, /*free_now=*/false);
          std::lock_guard<std::mutex> lock(g_mutex);
          ++g_epoch[dchunk];
        }
      }
    }
    ctx.r3.u32 = 1;  // we took ownership of the caller's block
    return;
  }

  if (!Enabled()) {
    __imp__sub_82990A58(ctx, base);
    return;
  }
  if (ours) {
    CheckChunkFreeHead(base, pool, chunk, guest_lr, "free entry");
  }
  // The record and the fill are the expensive half (a backtrace and a
  // block-sized memset per free); the watch does not need them, so they are
  // separately switchable. Marking below is NOT - without it the watch has no
  // free/live shadow and would sit silent forever.
  if (ours && PoisonEnabled()) {
    bool double_free = false;
    FreeRecord prev;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_freed.find(block);
      if (it != g_freed.end() && it->second.block_size == bsize) {
        double_free = true;
        prev = it->second;
      } else {
        FreeRecord rec;
        rec.lr = guest_lr;
        rec.block_size = bsize;
        rec.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        auto ep = g_epoch.find(chunk);
        rec.epoch = ep == g_epoch.end() ? 0 : ep->second;
        std::snprintf(rec.thread, sizeof(rec.thread), "%s", ThreadName());
        rec.frame_count = ::backtrace(rec.frames, kFreeFrames);
        g_freed[block] = rec;
      }
    }
    if (double_free) {
      REXLOG_ERROR(
          "heap-check: DOUBLE FREE: block 0x{:08X} (size {}) is already on the "
          "free list - freed first by guest lr=0x{:08X} on thread {}, now "
          "again from guest lr=0x{:08X} on thread {}.",
          block, bsize, prev.lr, prev.thread, guest_lr, ThreadName());
      Fail(base, pool, chunk, guest_lr);
    } else {
      // Fill past word 0: the allocator writes the free-list link there, and
      // the fresh-chunk carve rewrites word 0 of every block - so word 0 is the
      // one word we cannot own, and everything above it must survive untouched
      // until the block is handed out again.
      std::memset(base + block + 4, kPoison, bsize - 4);
    }
  }

  // Quarantine. The watch can only see a stale write if the block is still
  // free when it lands, and the pool hands blocks straight back out (LIFO), so
  // that window is on the order of the next allocation. Holding a bounded set
  // of freed blocks back from the allocator widens it by however long the queue
  // takes to drain - and a quarantined block is safer than a free one: the
  // allocator does not know it exists, so NOTHING may write to it, word 0
  // included.
  if (ours && QuarantineBytes() != 0) {
    std::memset(base + block, kPoison, bsize);
    WatchMark(block, bsize, /*free_now=*/true);
    const bool dtor = tl_in_instance_dtor;
    // Drain until under budget, not once per push: evicting a single block per
    // free bounds the queue's LENGTH, and the byte total then drifts with the
    // size mix. A quarantine that grows without limit would starve the pool and
    // manufacture failures that look exactly like the bug being hunted.
    std::vector<QuarantinedBlock> release;
    {
      std::lock_guard<std::mutex> lock(g_quarantine_mutex);
      auto& queue = dtor ? g_dtor_quarantine : g_quarantine;
      uint64_t& bytes = dtor ? g_dtor_quarantine_bytes : g_quarantine_bytes;
      if (dtor) {
        g_dtor_blocks.fetch_add(1, std::memory_order_relaxed);
      }
      queue.push_back({pool, block, bsize});
      bytes += bsize;
      while (bytes > QuarantineBytes() && !queue.empty()) {
        release.push_back(queue.front());
        bytes -= queue.front().size;
        queue.pop_front();
      }
    }
    for (const QuarantinedBlock& old : release) {
      // Unmark before the real free: the allocator writes the link word as it
      // pushes the block, and that write is legitimate.
      WatchMark(old.block, old.size, /*free_now=*/false);
      ctx.r3.u32 = old.pool;
      ctx.r4.u32 = old.block;
      __imp__sub_82990A58(ctx, base);
      const uint32_t old_chunk = ChunkOf(old.block);
      CheckChunkFreeHead(base, old.pool, old_chunk, guest_lr,
                         "quarantine release");
      WatchMark(old.block, old.size, /*free_now=*/true);
      if (REX_LOAD_U16(old_chunk + kChunkUsed) == 0) {
        WatchMark(old_chunk, kChunkSize, /*free_now=*/false);
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_epoch[old_chunk];
      }
    }
    // Whatever we actually released, the caller asked about ITS block and we
    // took ownership of it.
    ctx.r3.u32 = 1;
    return;
  }

  __imp__sub_82990A58(ctx, base);

  if (ours) {
    CheckChunkFreeHead(base, pool, chunk, guest_lr, "free exit");
    // Only now: the allocator writes the block's link word during the push, and
    // that write is legitimate.
    WatchMark(block, bsize, /*free_now=*/true);
    // Last block out: the chunk is now on the pool's free-chunk list and the
    // next bin to want a chunk can re-carve it at a different block size. Every
    // record for it is stale from here on, and the re-carve legitimately writes
    // a link word into every block in it.
    if (REX_LOAD_U16(chunk + kChunkUsed) == 0) {
      WatchMark(chunk, kChunkSize, /*free_now=*/false);
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_epoch[chunk];
    }
  }
}

// Sk8::WorldPresentation instance teardown. sub_82791CF8 allocates two u16
// arrays at self+496/+500 and publishes INTERIOR pointers into them
// (&A[idx], &B[idx]) into other objects' +64/+68; this frees both arrays
// without those holders being cleared, which is the dangling-alias setup the
// caught corruption fits. Flagging the frees it performs routes them into
// their own quarantine, where a stale write has minutes to land instead of a
// second - see g_dtor_quarantine.
extern "C" REX_FUNC(sub_82792040) {
  if (!HooksActive()) {
    __imp__sub_82792040(ctx, base);
    return;
  }
  const bool prev = tl_in_instance_dtor;
  tl_in_instance_dtor = true;
  __imp__sub_82792040(ctx, base);
  tl_in_instance_dtor = prev;
}
