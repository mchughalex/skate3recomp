#include "skate3_freecam_input.h"

#include <mutex>

#include <rex/logging.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

using rex::ui::VirtualKey;

namespace skate3 {
namespace freecam_input {

namespace {

// Mirrors the reduced freecam controls so we don't grab the whole modifier set
// unless actually in use. Indexed by VirtualKey.
std::mutex g_mutex;
bool g_keys[256] = {};
bool g_right_mouse_down = false;
int32_t g_mouse_dx = 0;
int32_t g_mouse_dy = 0;
int32_t g_prev_mouse_x = 0;
int32_t g_prev_mouse_y = 0;
bool g_registered = false;

class FreecamInputListener final : public rex::ui::WindowInputListener,
                                   public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const auto vk = e.virtual_key();
    if (static_cast<uint16_t>(vk) < 256) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_keys[static_cast<uint16_t>(vk)] = true;
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const auto vk = e.virtual_key();
    if (static_cast<uint16_t>(vk) < 256) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_keys[static_cast<uint16_t>(vk)] = false;
    }
  }

  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_right_mouse_down = true;
      g_prev_mouse_x = e.x();
      g_prev_mouse_y = e.y();
      g_mouse_dx = 0;
      g_mouse_dy = 0;
    }
  }

  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_right_mouse_down = false;
    }
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_right_mouse_down) {
      g_mouse_dx += e.x() - g_prev_mouse_x;
      g_mouse_dy += e.y() - g_prev_mouse_y;
    }
    g_prev_mouse_x = e.x();
    g_prev_mouse_y = e.y();
  }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    // A window can lose focus mid-keypress without the matching key-up; clear
    // the held set so freecam doesn't stick (same failure mode the MnK driver
    // guards against).
    std::lock_guard<std::mutex> lock(g_mutex);
    for (bool& k : g_keys) {
      k = false;
    }
    g_right_mouse_down = false;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
  }
};

FreecamInputListener* g_listener = nullptr;

}  // namespace

void Register(rex::ui::Window* window) {
  if (!window || g_registered) {
    return;
  }
  auto* listener = new FreecamInputListener();
  window->AddInputListener(listener, 0xFFFFU);
  window->AddListener(listener);
  g_listener = listener;
  g_registered = true;
  REXLOG_INFO("native-scene freecam: host input capture registered");
}

void Unregister(rex::ui::Window* window) {
  if (!g_registered || !window) {
    return;
  }
  window->RemoveInputListener(g_listener);
  window->RemoveListener(g_listener);
  delete g_listener;
  g_listener = nullptr;
  g_registered = false;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (bool& k : g_keys) {
    k = false;
  }
  g_right_mouse_down = false;
  g_mouse_dx = 0;
  g_mouse_dy = 0;
}

bool IsKeyDown(rex::ui::VirtualKey vk) {
  const uint16_t idx = static_cast<uint16_t>(vk);
  if (idx >= 256) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_keys[idx];
}

void DrainMouseDelta(int32_t& out_dx, int32_t& out_dy) {
  std::lock_guard<std::mutex> lock(g_mutex);
  out_dx = g_mouse_dx;
  out_dy = g_mouse_dy;
  g_mouse_dx = 0;
  g_mouse_dy = 0;
}

}  // namespace freecam_input
}  // namespace skate3
