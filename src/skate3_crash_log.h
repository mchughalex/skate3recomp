#pragma once
// Crash diagnostics for Skate 3.
//
// The recompiled game can terminate on a hard fault (segfault, abort, bad
// trap) with no user-facing trace. This installs signal/exception handlers at
// app startup that turn the fault into a single copyable block written to
// <app root>/crash_log.txt (and mirrored to the log/stderr), then exits the
// process after flushing so no state is lost.
//
// Writing from a crash handler is not truly async-signal-safe in general, but
// a dedicated FILE opened at boot (no allocation, no locking at fault time) is
// the standard pragmatic approximation for a diagnostics feature like this.

namespace skate3 {
namespace crashlog {

// Installs the crash handlers. Safe to call once at startup; no-op if already
// installed. Not thread-safe (call from the main/UI thread during setup).
void Install();

}  // namespace crashlog
}  // namespace skate3