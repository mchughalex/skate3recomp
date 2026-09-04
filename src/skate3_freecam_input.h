#pragma once
// Freecam host input capture (cross-platform).
//
// The guest-thread freecam (UpdateFreecam) needs continuous, thread-safe key
// and mouse input, but the rexglue input events arrive on the UI thread. The
// cross-platform way to source that input is to listen to the window's input
// events, mirror the live key/mouse state into a mutex-guarded snapshot, and
// let the guest thread read it back under the same lock.
//
// This is deliberately independent of the MnK controller-emulation driver
// (mnk_mode): that driver only tracks keys once mnk_mode is enabled and would
// additionally inject a fake 360 controller into the guest, so it is wrong to
// reuse for freecam. Keys are indexed by rex::ui::VirtualKey, whose values are
// identical to the Win32 VK_* codes the original freecam used.
#include <cstdint>
#include <mutex>

#include <rex/ui/window_listener.h>

namespace rex {
namespace ui {
class Window;
}
}  // namespace rex

namespace skate3 {
namespace freecam_input {

// Registers the capture with the window (UI thread; delivers input events).
// Holds no persistent allocation: the capture lives for the process.
void Register(rex::ui::Window* window);
void Unregister(rex::ui::Window* window);

// Guest-thread accessors. Keys are the persisted held-state; mouse deltas are
// drained (returned and zeroed) so each guest frame applies them once, matching
// the per-frame semantics of the original freecam.
bool IsKeyDown(rex::ui::VirtualKey vk);
void DrainMouseDelta(int32_t& out_dx, int32_t& out_dy);

}  // namespace freecam_input
}  // namespace skate3
