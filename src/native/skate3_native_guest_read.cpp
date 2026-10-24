// Shared sample-vertex decode + skin + spread helpers (see the header).

#include "native/skate3_native_guest_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "skate3_init.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <csetjmp>
#include <mutex>

#include <rex/exception_handler.h>
#endif

namespace skate3::native_scene {

// Lock-free guarded bulk copy for reads of guest payloads (shared here so
// every TU uses the one correctly-built guard). GuestRangeReadable's
// VirtualQuery loop takes the
// process VAD lock, which the guest streaming threads hammer exactly while
// panning streams the world in; every render-thread decode then queues
// behind them (multi-ms stalls; same lock as the 3 fps PERF TRAP). An
// SEH-guarded memcpy costs nothing in the good case and fails cleanly if
// streaming decommitted the range between capture and decode. Own function,
// no C++ objects: SEH cannot share a frame with unwinding.
#if defined(_WIN32)
// TWO TRAPS PROVEN FROM CRASH DUMPS OF THE REGISTRY-PREWARM PROBE; both
// silently delete the guard and let the AV kill the process:
// 1. clang marks std::memcpy nounwind, concludes the __except is
//    unreachable, and compiles the whole function to `jmp memcpy` (seen in
//    the shipped binary). Calling through a VOLATILE function pointer makes
//    the callee opaque so the SEH scope must be kept.
// 2. When this function is INLINED into a caller using the C++ EH
//    personality (__CxxFrameHandler3), the __except scope is dropped in the
//    merge, hence the noinline.
// This means plain `__try { std::memcpy(...) } __except` NEVER protected
// anything in optimized builds; any future guarded read must go through
// this function.
namespace {
typedef void* (*GuestMemcpyFn)(void*, const void*, size_t);
volatile GuestMemcpyFn g_guest_memcpy_fn = std::memcpy;
}  // namespace
__declspec(noinline) bool GuestTryCopy(void* dst, const void* src, size_t size) {
  __try {
    g_guest_memcpy_fn(dst, src, size);
    return true;
  } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
               GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {  // NOLINT
    return false;
  }
}
#else
// POSIX guard with the same clean-failure semantics as the SEH path: a
// handler registered in the runtime's exception chain (SIGSEGV/SIGBUS both
// arrive as Code::kAccessViolation) siglongjmps back here when the fault
// happened inside the guarded copy on this thread; unrelated faults are
// declined so the runtime's own handlers (write watches, null traps) keep
// working. The previous raw std::memcpy crashed the first macOS
// native-render session (SIGBUS in _platform_memmove under
// OnRenderModeSceneExit: streaming decommitted the captured range between
// capture and decode, exactly the case the Windows guard exists for).
namespace {
thread_local volatile bool tl_in_guest_copy = false;
thread_local sigjmp_buf tl_guest_copy_jmp;

bool GuestCopyFaultHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (!tl_in_guest_copy ||
      ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;  // not ours - keep dispatching down the chain
  }
  tl_in_guest_copy = false;
  // Unwinds the signal frame directly; the kernel-blocked signal is
  // unblocked by the sigsetjmp fault branch (savemask=0 keeps the hot path
  // free of a per-call sigprocmask syscall).
  siglongjmp(tl_guest_copy_jmp, 1);
}

void EnsureGuestCopyHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::arch::ExceptionHandler::Install(GuestCopyFaultHandler, nullptr);
  });
}
}  // namespace

bool GuestTryCopy(void* dst, const void* src, size_t size) {
  EnsureGuestCopyHandler();
  if (sigsetjmp(tl_guest_copy_jmp, 0) != 0) {
    // Faulted mid-copy and jumped out of the signal handler: the signal is
    // still in this thread's blocked mask (handler entry blocked it, the
    // longjmp skipped the normal return that would restore it). Rare path -
    // the syscall is fine here.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGSEGV);
    sigaddset(&set, SIGBUS);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    return false;
  }
  tl_in_guest_copy = true;
  std::memcpy(dst, src, size);
  tl_in_guest_copy = false;
  return true;
}

// ---- Raw-load read-fault recovery (see the header contract) ---------------
// The handler must be async-signal-safe: TLS reads, atomics and the
// mprotect/clock_gettime syscalls only. It resumes the interrupted load in
// place (registers untouched, pc unchanged), so unlike the siglongjmp guard
// above it never crosses C++ scopes and imposes no constraints on the armed
// code. Recovery restores READ access only: a page the runtime keeps
// write-protected (write watches) still traps guest writes exactly as
// before.
namespace {

// Full span of the runtime's guest views measured from the virtual membase:
// 4 GB of virtual address space plus the 512 MB physical-raw view mapped
// directly above it (the SDK maps both from one backing object).
constexpr uint64_t kGuestSpanBytes = 0x120000000ull;

thread_local int tl_read_recovery_depth = 0;
thread_local uint8_t* tl_read_recovery_base = nullptr;
// Livelock breaker: a read fault that RECURS on the same page immediately
// after a successful recovery means read access cannot actually be restored
// there (e.g. SIGBUS from the backing object rather than page protection).
// Distinct pages, and the same page faulting again later (streaming re-
// revoked it), reset the streak.
thread_local uintptr_t tl_read_recovery_page = 0;
thread_local uint32_t tl_read_recovery_streak = 0;
thread_local uint64_t tl_read_recovery_last_ns = 0;

std::atomic<uint64_t> g_read_recoveries{0};
size_t g_host_page_size = 4096;

bool GuestReadFaultHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (tl_read_recovery_depth <= 0 || tl_in_guest_copy ||
      ex->code() != rex::arch::Exception::Code::kAccessViolation ||
      ex->access_violation_operation() !=
          rex::arch::Exception::AccessViolationOperation::kRead) {
    return false;  // not ours - keep dispatching down the chain
  }
  uint8_t* base = tl_read_recovery_base;
  const uint64_t fault = ex->fault_address();
  const uint64_t lo = uint64_t(reinterpret_cast<uintptr_t>(base));
  if (base == nullptr || fault < lo || fault - lo >= kGuestSpanBytes) {
    return false;
  }
  const uintptr_t page =
      uintptr_t(fault) & ~(uintptr_t(g_host_page_size) - 1);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  if (page == tl_read_recovery_page &&
      now_ns - tl_read_recovery_last_ns < 10000000ull) {
    if (++tl_read_recovery_streak > 16) {
      return false;  // recovery is not sticking on this page: die loudly
    }
  } else {
    tl_read_recovery_page = page;
    tl_read_recovery_streak = 0;
  }
  tl_read_recovery_last_ns = now_ns;
  if (mprotect(reinterpret_cast<void*>(page), g_host_page_size, PROT_READ) != 0) {
    return false;
  }
  g_read_recoveries.fetch_add(1, std::memory_order_relaxed);
  return true;  // resume: the faulted load retries against the readable page
}

void EnsureGuestReadRecoveryHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) {
      g_host_page_size = size_t(ps);
    }
    rex::arch::ExceptionHandler::Install(GuestReadFaultHandler, nullptr);
  });
}

}  // namespace

GuestReadRecoveryScope::GuestReadRecoveryScope(uint8_t* base) {
  EnsureGuestReadRecoveryHandler();
  if (tl_read_recovery_depth++ == 0) {
    tl_read_recovery_base = base;
  }
}

GuestReadRecoveryScope::~GuestReadRecoveryScope() {
  if (--tl_read_recovery_depth == 0) {
    tl_read_recovery_base = nullptr;
  }
}

void ArmGuestReadRecoveryForThread(uint8_t* base) {
  if (tl_read_recovery_depth == 0) {
    EnsureGuestReadRecoveryHandler();
    tl_read_recovery_depth = 1;
    tl_read_recovery_base = base;
  }
}

uint64_t GuestReadRecoveryCount() {
  return g_read_recoveries.load(std::memory_order_relaxed);
}
#endif

float GuestHalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1F;
  const uint32_t man = h & 0x3FF;
  if (exp == 0) return std::bit_cast<float>(sign);  // denorms ~0
  if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u);
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (man << 13));
}

float BindDiag(const DrawItem& item) {
  float d2 = 0.0f;
  for (int a = 0; a < 3; ++a) {
    const float d = item.bbox_max[a] - item.bbox_min[a];
    d2 += d * d;
  }
  return std::sqrt(d2);
}

namespace {

bool PosFinite(const float p[3]) {
  return p[0] > -1e7f && p[0] < 1e7f && p[1] > -1e7f && p[1] < 1e7f &&
         p[2] > -1e7f && p[2] < 1e7f;
}

}  // namespace

bool ReadSkinVertGuest(uint8_t* base, const DrawItem& item, uint32_t vtx,
                       SkinSampleVert* out) {
  const uint32_t v = item.vb_addr + vtx * item.stride;
  const uint32_t pa = v + item.pos_offset;
  float* p = out->p;
  switch (item.pos_fmt) {
    case 57:
      for (int a = 0; a < 3; ++a) {
        const uint32_t u = REX_LOAD_U32(pa + a * 4);
        std::memcpy(&p[a], &u, 4);
      }
      break;
    case 32:
      for (int a = 0; a < 3; ++a) {
        p[a] = GuestHalfToFloat(uint16_t(REX_LOAD_U16(pa + a * 2)));
      }
      break;
    case 26: {
      constexpr float kScale = 2.0f / 32767.0f;
      for (int a = 0; a < 3; ++a) {
        p[a] = int16_t(REX_LOAD_U16(pa + a * 2)) * kScale + (a == 1 ? 0.8f : 0.0f);
      }
      break;
    }
    default:
      return false;
  }
  out->pos_finite = PosFinite(p);
  if (item.bw_offset != 0 && item.bi_offset != 0) {
    // u8x4 attributes are big-endian per 32-bit word: component k is byte
    // (24 - 8k) of the host-order load.
    const uint32_t bw = REX_LOAD_U32(v + item.bw_offset);
    const uint32_t bi = REX_LOAD_U32(v + item.bi_offset);
    for (int k = 0; k < 4; ++k) {
      out->w[k] = uint8_t((bw >> (24 - 8 * k)) & 0xFF);
      out->bone[k] = uint8_t((bi >> (24 - 8 * k)) & 0xFF);
    }
  } else {
    std::memset(out->w, 0, sizeof(out->w));
    std::memset(out->bone, 0, sizeof(out->bone));
  }
  return true;
}

bool ReadSkinSamplesGuest(uint8_t* base, const DrawItem& item, uint32_t n,
                          SkinSampleVert* out) {
  if (item.stride == 0) {
    return false;
  }
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2 || n < 2) {
    return false;
  }
  for (uint32_t s = 0; s < n; ++s) {
    if (!ReadSkinVertGuest(base, item, s * (count - 1) / (n - 1), &out[s])) {
      return false;
    }
  }
  return true;
}

uint32_t SkinPointHostRows(const SkinSampleVert& sv, const float* rows,
                           size_t rows_floats, float q[3]) {
  uint32_t total = 0;
  q[0] = q[1] = q[2] = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const uint32_t w = sv.w[k];
    if (w == 0) continue;
    const uint32_t r0 = 3u * sv.bone[k];
    if ((r0 + 3) * 4 > rows_floats) continue;
    total += w;
    for (int a = 0; a < 3; ++a) {
      const float* row = rows + (r0 + uint32_t(a)) * 4;
      q[a] += float(w) * (row[0] * sv.p[0] + row[1] * sv.p[1] + row[2] * sv.p[2] +
                          row[3]);
    }
  }
  if (total != 0) {
    for (int a = 0; a < 3; ++a) q[a] /= float(total);
  }
  return total;
}

uint32_t SkinPointBankRows(uint8_t* base, uint32_t bank, uint32_t pb,
                           const SkinSampleVert& sv, float q[3], bool* rows_sane) {
  uint32_t total = 0;
  q[0] = q[1] = q[2] = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const uint32_t w = sv.w[k];
    if (w == 0) continue;
    const uint32_t r0 = pb + 3 * uint32_t(sv.bone[k]);
    if (r0 + 3 > 256) continue;
    total += w;
    for (int a = 0; a < 3; ++a) {
      float row[4];
      for (int i = 0; i < 4; ++i) {
        row[i] = std::bit_cast<float>(REX_LOAD_U32(bank + ((r0 + a) * 4 + i) * 4));
      }
      // A weighted bone's rotation row must be rotation-shaped (norm near
      // the entity scale). The vehicle-flick captures carried WORLD
      // POSITIONS in the rotation slots (norm ~140); those can still
      // project on-screen and pass the geometric gate.
      const float rn = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
      if (rn < 0.04f || rn > 25.0f) {
        *rows_sane = false;
      }
      q[a] += float(w) * (row[0] * sv.p[0] + row[1] * sv.p[1] + row[2] * sv.p[2] +
                          row[3]);
    }
  }
  if (total != 0) {
    for (int a = 0; a < 3; ++a) q[a] /= float(total);
  }
  return total;
}

int SkinnedSpreadHostRows(const SkinSampleVert* sv, uint32_t n, const float* rows,
                          size_t rows_floats, int min_n, bool garbage_fails,
                          float* out_spread, int* out_n) {
  float qmin[3] = {1e9f, 1e9f, 1e9f};
  float qmax[3] = {-1e9f, -1e9f, -1e9f};
  int weighted = 0;
  for (uint32_t s = 0; s < n; ++s) {
    if (!sv[s].pos_finite) {
      if (garbage_fails) {
        return -1;  // NaN/garbage positions mid-sim-write
      }
      continue;
    }
    float q[3];
    if (SkinPointHostRows(sv[s], rows, rows_floats, q) == 0) {
      continue;
    }
    ++weighted;
    for (int a = 0; a < 3; ++a) {
      qmin[a] = std::min(qmin[a], q[a]);
      qmax[a] = std::max(qmax[a], q[a]);
    }
  }
  if (out_n) {
    *out_n = weighted;
  }
  if (weighted < min_n) {
    return 0;  // nothing to judge
  }
  const float dx = qmax[0] - qmin[0];
  const float dy = qmax[1] - qmin[1];
  const float dz = qmax[2] - qmin[2];
  *out_spread = std::sqrt(dx * dx + dy * dy + dz * dz);
  return 1;
}

}  // namespace skate3::native_scene
