// backtrace()/backtrace_symbols_fd() are GNU extensions in <execinfo.h>; make
// sure their declarations are visible regardless of the translation unit's
// feature-test macros. Must precede every standard header.
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "skate3_crash_log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <exception>

#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#if defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace skate3 {
namespace crashlog {

namespace {

std::atomic<bool> g_installed{false};
std::atomic<bool> g_dumping{false};

FILE* g_crash_fd = nullptr;
FILE* g_crash_fd_mirror = nullptr;  // second copy in the user root (discoverable)

#if defined(__linux__) || defined(__APPLE__)
const char* SignalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE:  return "SIGFPE (floating-point exception)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGQUIT: return "SIGQUIT (quit)";
    case SIGTRAP: return "SIGTRAP (breakpoint trap)";
    case SIGSYS:  return "SIGSYS (bad syscall)";
    case SIGPIPE: return "SIGPIPE (broken pipe)";
    default:      return nullptr;
  }
}
#endif

// Crash-safe raw writer: ONLY POSIX async-signal-safe calls (write()). No
// stdio/fprintf (stdio locks can be held by the very thread that faulted), no
// malloc, no localtime/strftime. A stale FILE buffer or a lock held at the
// crash point can otherwise silently swallow the whole report -- which is
// exactly what produced the 0-byte crash_log.txt we observed.
void RawWriteFd(int fd, const char* s, size_t n) {
#if defined(__linux__) || defined(__APPLE__)
  if (n == 0 || fd < 0) {
    return;
  }
  while (n > 0) {
    ssize_t r = ::write(fd, s, n);
    if (r <= 0) {
      break;  // EINTR/EAGAIN from a signal-context write: give up, don't loop.
    }
    s += r;
    n -= static_cast<size_t>(r);
  }
#else
  static_cast<void>(fd); static_cast<void>(s); static_cast<void>(n);
#endif
}

void RawWriteCStr(int fd, const char* s) {
  if (s != nullptr) {
    RawWriteFd(fd, s, std::strlen(s));
  }
}

// Write the same bytes to every open crash sink (primary + user-root mirror).
// Async-signal-safe: only raw write() to pre-opened fds.
void WriteAll(const char* s, size_t n) {
  for (FILE* f : {g_crash_fd, g_crash_fd_mirror}) {
    if (f != nullptr) {
      const int fdo = fileno(f);
      if (fdo >= 0) {
        RawWriteFd(fdo, s, n);
      }
    }
  }
}

void WriteAllCStr(const char* s) {
  if (s != nullptr) {
    WriteAll(s, std::strlen(s));
  }
}

// Async-signal-safe thread-id (Linux: syscall(SYS_gettid)). Returns a short
// decimal string in `out` (no trailing NUL guarantee; returns length).
size_t FormatTid(char* out, size_t cap) {
  long tid = 0;
#if defined(__linux__)
  tid = static_cast<long>(::syscall(SYS_gettid));
#endif
  unsigned v = tid > 0 ? static_cast<unsigned>(tid) : 0u;
  char tmp[24];
  int i = static_cast<int>(sizeof(tmp));
  if (v == 0) {
    tmp[--i] = '0';
  } else {
    while (v != 0 && i > 0) {
      tmp[--i] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
  }
  const size_t len = static_cast<size_t>(sizeof(tmp) - i);
  size_t put = len < cap ? len : cap;
  std::memcpy(out, tmp + i, put);
  return put;
}

// Signal-safe value formatter (decimal) into a caller buffer; returns bytes.
size_t FormatUInt(char* buf, size_t cap, unsigned v) {
  char tmp[24];
  int i = static_cast<int>(sizeof(tmp));
  if (v == 0) {
    tmp[--i] = '0';
  } else {
    while (v != 0 && i > 0) {
      tmp[--i] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
  }
  size_t len = static_cast<size_t>(sizeof(tmp) - i);
  size_t put = len < cap ? len : cap;
  std::memcpy(buf, tmp + i, put);
  return put;
}

// Signal-safe lowercase hex formatter for 64-bit values (fault addr / pc are
// full 64-bit; the previous decimal FormatUInt truncated them to 32 bits).
size_t FormatHex64(char* buf, size_t cap, std::uint64_t v) {
  char tmp[20];
  int i = static_cast<int>(sizeof(tmp));
  if (v == 0) {
    tmp[--i] = '0';
  } else {
    while (v != 0 && i > 0) {
      const unsigned d = static_cast<unsigned>(v & 0xF);
      tmp[--i] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
      v >>= 4;
    }
  }
  size_t len = static_cast<size_t>(sizeof(tmp) - i);
  size_t put = len < cap ? len : cap;
  std::memcpy(buf, tmp + i, put);
  return put;
}

void DumpRecord(int sig, const char* reason) {
  // If no crash sink is open (Install never reached, or fopen failed), fall
  // back to a best-effort stderr trace so we never fail silent. stderr is
  // always open; this uses only raw write() so it stays async-signal-safe.
  const bool have_sink = (g_crash_fd != nullptr || g_crash_fd_mirror != nullptr);
  if (!have_sink) {
    RawWriteCStr(STDERR_FILENO,
                 "skate3: crash before crashlog sink was armed; raw trace follows\n");
#if defined(__linux__) || defined(__APPLE__)
    void* frames[64];
    const int n = backtrace(frames, 64);
    if (n > 0) {
      backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
#endif
    return;
  }

  time_t now = time(nullptr);

  char num[24];
  size_t tl = FormatUInt(num, sizeof(num),
                         static_cast<unsigned>(now != static_cast<time_t>(-1) ? now : 0));
  char tid[24];
  size_t xl = FormatTid(tid, sizeof(tid));

  // Write the reason/signal header FIRST, synchronously, before the riskier
  // backtrace. Even if backtrace() itself faults, we keep the essential record.
  WriteAllCStr("==========================================================\n"
               "Skate 3 crash report\n"
               "time:      epoch ");
  WriteAll(num, tl);
  WriteAllCStr("\ntid:       ");
  WriteAll(tid, xl);
  WriteAllCStr("\nreason:    ");
  WriteAllCStr(reason != nullptr ? reason : "unknown");
  WriteAllCStr("\nfault:     ");
#if defined(__linux__) || defined(__APPLE__)
  const char* sn = SignalName(sig);
  if (sn != nullptr) {
    WriteAllCStr(sn);
  } else {
    char snb[24];
    size_t sl = FormatUInt(snb, sizeof(snb), static_cast<unsigned>(sig));
    WriteAllCStr("signal ");
    WriteAll(snb, sl);
  }
#else
  char exc[24];
  size_t el = FormatUInt(exc, sizeof(exc), static_cast<unsigned>(sig));
  WriteAllCStr("exception code 0x");
  WriteAll(exc, el);
#endif
  WriteAllCStr("# hint: symbolize frames offline -- see tools/symbolize_crash.py\n"
               "backtrace:\n");

#if defined(__linux__) || defined(__APPLE__)
  void* frames[64];
  const int n = backtrace(frames, 64);
  // backtrace_symbols_fd writes straight to the fd via write(); signal-safe.
  if (n > 0) {
    for (FILE* f : {g_crash_fd, g_crash_fd_mirror}) {
      if (f != nullptr) {
        const int fdo = fileno(f);
        if (fdo >= 0) {
          backtrace_symbols_fd(frames, n, fdo);
        }
      }
    }
  }
#endif

  WriteAllCStr("==========================================================\n"
               "keep this block to report the crash\n");
  // No fflush needed -- we only used unbuffered write().
}

// Prior handler saved from sigaction for the signals we chain ourselves.
// Only populated for signals outside the SDK's exception set (the SDK covers
// SIGILL/SIGSEGV/SIGBUS; we chain everything else).
constexpr size_t kMaxChained = 8;
struct ChainedSignal {
  int sig;
  struct sigaction prior{};
};
ChainedSignal g_chained[kMaxChained];
size_t g_chained_count = 0;

// Called by our OWN sigaction handler only. Logs, then forwards to the prior
// handler that was installed before us (matching the SDK's own
// DispatchUnhandledSignal chain semantics) so we never swallow/replace a
// handler that was already installed and working.
void HandleCrash(int sig) {
  // Suppress nested faults from the handler itself.
  if (g_dumping.exchange(true)) {
    DumpRecord(sig, "crash signal (nested)");
    _Exit(128 + sig);
  }
  DumpRecord(sig, "crash signal");

  // Forward to whatever was installed before us (if any). This restores the
  // pre-existing behavior instead of forcibly _Exit()ing every time -- the
  // original failure mode of this file (replacing the SDK's own handler).
  struct sigaction* prior = nullptr;
  for (size_t i = 0; i < g_chained_count; ++i) {
    if (g_chained[i].sig == sig) {
      prior = &g_chained[i].prior;
      break;
    }
  }
  if (prior != nullptr) {
    if ((prior->sa_flags & SA_SIGINFO) && prior->sa_sigaction) {
      // Can't recover the original siginfo/ucontext here; a SA_SIGINFO prior
      // handler is the SDK's -- let it re-raise via its own saved handler.
      signal(sig, SIG_DFL);
      raise(sig);
      _Exit(128 + sig);
    }
    if (prior->sa_handler == SIG_IGN) {
      return;
    }
    if (prior->sa_handler != SIG_DFL && prior->sa_handler) {
      prior->sa_handler(sig);
      return;
    }
  }
  // No prior handler: restore the default so the OS/core tooling still sees a
  // real crash rather than our _Exit (the original game's signal path).
  signal(sig, SIG_DFL);
  raise(sig);
  _Exit(128 + sig);
}

// The SDK routes a failed/terminating guest thread through std::terminate()
// (see rexglue-sdk kernel/crt/threading.cpp). That path does not deliver a
// signal we intercept, so install a terminate handler too.
void HandleTerminate() noexcept {
  if (g_dumping.exchange(true)) {
    return;
  }
  DumpRecord(0, "std::terminate (uncaught exception / guest thread died)");
  rex::FlushLogging();
  std::fflush(nullptr);
  g_crash_fd = nullptr;
  _Exit(134);  // 128 + SIGABRT
}

void InstallHandler(int sig, void (*fn)(int)) {
#if defined(__linux__) || defined(__APPLE__)
  struct sigaction sa{};
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = fn;
  sigemptyset(&sa.sa_mask);
  // SA_NODEFER so a repeated fault still re-enters (guarded by g_dumping);
  // SA_RESETHAND as a backstop against infinite recursion in the handler.
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  // Save whatever was there so HandleCrash can chain onto it and we never
  // permanently replace a handler the game/SDK already installed (the SDK
  // covers SIGILL/SIGSEGV/SIGBUS via its own ExceptionHandler table).
  struct sigaction old{};
  sigaction(sig, &sa, &old);
  if (g_chained_count < kMaxChained) {
    g_chained[g_chained_count].sig = sig;
    g_chained[g_chained_count].prior = old;
    ++g_chained_count;
  }
#else
  static_cast<void>(sig);
  static_cast<void>(fn);
#endif
}

// Registered into the SDK's cooperative exception table (rex::arch::
// ExceptionHandler) so we run AFTER the SDK's own ExceptionHandlerCallback and
// never replace it. THIS RUNS INSIDE THE SIGSEGV/SIGILL/SIGBUS SIGNAL HANDLER
// FRAME (ExceptionHandlerCallback is the sa_sigaction; it calls our handler
// synchronously), so it MUST be async-signal-safe: raw write() only, NO stdio
// (fflush can block on a FILE lock the faulting thread itself held), NO
// rex::FlushLogging / no non-safe calls. We log the fault address + PC, then
// return false so the SDK proceeds with its normal unhandled-signal path
// (re-raise with the original handler -> real core dump).
bool HandleSdkException(rex::arch::Exception* ex, void* /*data*/) {
#if defined(__linux__) || defined(__APPLE__)
  if (g_dumping.exchange(true)) {
    // Nested fault while we were already dumping (e.g. backtrace_symbols_fd
    // itself faulting): re-raise immediately rather than recurse.
    signal(SIGSEGV, SIG_DFL);
    ::raise(SIGSEGV);
    _Exit(128 + SIGSEGV);
  }
  const bool have_sink = (g_crash_fd != nullptr || g_crash_fd_mirror != nullptr);
  if (!have_sink) {
    RawWriteCStr(STDERR_FILENO,
                 "skate3: SDK fault before crashlog sink was armed; raw trace follows\n");
#if defined(__linux__) || defined(__APPLE__)
    void* frames[64];
    const int n = backtrace(frames, 64);
    if (n > 0) {
      backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
#endif
    return false;
  }

  time_t now = time(nullptr);
  char num[24];
  size_t tl = FormatUInt(num, sizeof(num),
                         static_cast<unsigned>(now != static_cast<time_t>(-1) ? now : 0));
  char tid[24];
  size_t xl = FormatTid(tid, sizeof(tid));

  WriteAllCStr("==========================================================\n"
               "Skate 3 crash report\n"
               "time:      epoch ");
  WriteAll(num, tl);
  WriteAllCStr("\ntid:       ");
  WriteAll(tid, xl);
  WriteAllCStr("\nreason:    SDK fault");
  WriteAllCStr("\nfault:     SDK exception ");
  WriteAllCStr(ex->code() == rex::arch::Exception::Code::kAccessViolation
                   ? "access violation"
                   : "illegal instruction");
  char ahex[24];
  size_t xal = FormatHex64(ahex, sizeof(ahex), ex->fault_address());
  WriteAllCStr("\nfault addr: 0x");
  WriteAll(ahex, xal);
  char phex[24];
  size_t xpl = FormatHex64(phex, sizeof(phex), ex->pc());
  WriteAllCStr("\npc:        0x");
  WriteAll(phex, xpl);
  WriteAllCStr("# hint: symbolize frames offline -- see tools/symbolize_crash.py\n"
               "backtrace:\n");
  void* frames[64];
  const int n = backtrace(frames, 64);
  if (n > 0) {
    for (FILE* f : {g_crash_fd, g_crash_fd_mirror}) {
      if (f != nullptr) {
        const int fdo = fileno(f);
        if (fdo >= 0) {
          backtrace_symbols_fd(frames, n, fdo);
        }
      }
    }
  }
  WriteAllCStr("==========================================================\n"
               "keep this block to report the crash\n");
  // Do NOT flush stdio rex logging here (async-signal-safety). The record is
  // written via raw write() to the crash fd, so nothing more is needed. If we
  // returned false, the SDK re-raises with the original handler for a real
  // core dump.
  return false;  // unhandled: let the SDK continue to the original handler
#else
  static_cast<void>(ex);
  return false;
#endif
}

}  // namespace

void Install() {
  if (g_installed.exchange(true)) {
    return;
  }

  // Open crash sinks: primary in the user data root (beside settings.toml /
  // saves, the natural place to look on crash) and a mirror next to the
  // executable (where the user launched from). Each sink is independent:
  // if one fails, the other still works.
  const std::filesystem::path user_dir =
      rex::filesystem::GetUserFolder() / "skate3";
  std::filesystem::path primary_path;
  FILE* pf = nullptr;
  if (!user_dir.empty()) {
    std::filesystem::create_directories(user_dir);
    primary_path = user_dir / "crash_log.txt";
    pf = std::fopen(primary_path.string().c_str(), "w");
    if (pf != nullptr) {
      g_crash_fd = pf;
    } else {
      REXLOG_WARN("crashlog: could not open {} for crash output",
                  primary_path.string());
    }
  }

  const std::filesystem::path mirror_path =
      rex::filesystem::GetAppRootFolder() / "crash_log.txt";
  FILE* mf = std::fopen(mirror_path.string().c_str(), "w");
  if (mf != nullptr) {
    g_crash_fd_mirror = mf;
  } else {
    REXLOG_WARN("crashlog: could not open {} for crash mirror",
                mirror_path.string());
  }

  if (pf == nullptr && (g_crash_fd_mirror == nullptr)) {
    // Both sinks failed; install a signal handler that at least traces to
    // stderr so the crash isn't completely silent.
    REXLOG_WARN("crashlog: no crash output sinks available; stderr fallback "
                "only");
  }

  // Install-marker written to every available sink so a non-crashing run is
  // distinguishable (the "armed at ..." header). Use safe stdio (we're in
  // normal startup context here, not a signal handler).
  {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    char ms[64] = "unknown";
    if (::localtime_r(&now, &tm_buf) != nullptr) {
      std::strftime(ms, sizeof(ms), "%Y-%m-%d %H:%M:%S", &tm_buf);
    }
    for (FILE* sink : {g_crash_fd, g_crash_fd_mirror}) {
      if (sink != nullptr) {
        std::fprintf(sink, "Skate 3 crashlog armed at %s -- no handled crash "
                           "yet\n"
                           "# symbolize offline: tools/symbolize_crash.py "
                           "crash_log.txt\n",
                     ms);
        std::fflush(sink);
      }
    }
  }

  const char* sink_list = (g_crash_fd && g_crash_fd_mirror)
                              ? "two sinks (user root + build dir)"
                              : (g_crash_fd ? "primary (user root)" : "mirror "
                                                              "(build dir)");
  REXLOG_INFO("crashlog: crash reports will be written to {}", sink_list);

#if defined(__linux__) || defined(__APPLE__)
  // SIGILL/SIGSEGV/SIGBUS are owned by the rexglue SDK's cooperative exception
  // machinery (rex::arch::ExceptionHandler, which rexes mmio_handler and the
  // runtime rely on). We MUST chain into that instead of blind sigaction(),
  // which would replace the SDK's handler and destroy its context capture. Do
  // that first, then add our own standalone (chained) handlers only for the
  // signals the SDK does not cover.
  rex::arch::ExceptionHandler::Install(&HandleSdkException, nullptr);
  InstallHandler(SIGABRT, HandleCrash);
  InstallHandler(SIGFPE, HandleCrash);
  InstallHandler(SIGQUIT, HandleCrash);
  InstallHandler(SIGTRAP, HandleCrash);
  InstallHandler(SIGSYS, HandleCrash);
  InstallHandler(SIGPIPE, HandleCrash);
  // std::terminate handler (guest thread die / uncaught exception).
  std::set_terminate(HandleTerminate);
#else
  REXLOG_INFO("crashlog: hard-fault handler not installed on this platform");
#endif
}

}  // namespace crashlog
}  // namespace skate3