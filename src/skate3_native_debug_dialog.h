#pragma once

// F12 native-render debug menu: live controls for every native renderer
// subsystem (all hot-reload cvars), grouped by intent (capture tools,
// image quality, lighting, shadows, content, diagnostics), used to bisect
// visual regressions in-game without rebuilds. Includes the max-quality
// photo-mode toggle for showcase/promo recording.

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

// Debug stress driver: when skate3_native_render_scene_maxq_cycle > 0,
// toggles the max-quality photo mode every N native-scene frames. Called
// once per frame from the scene renderer (works without the F12 dialog).
void MaxQualityAutoCycle(uint64_t frame_number);

class NativeDebugDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit NativeDebugDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
};

// Standalone PERF menu (F3): MangoHUD-style performance dashboard, opened
// and closed by the F3 key. Fully independent of the F12 debug menu.
class PerformanceDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PerformanceDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
};

// Tiny top-right corner readout of which renderer produced the last
// presented frame: NATIVE (native scene renderer) or EMULATED (Xenos
// GPU emulation; menus/loading yields, F5 off). Input-transparent, no
// continuous repaint (updates ride the guest frame paints); off by
// default, shown via skate3_native_render_mode_indicator (hot), and
// force-shown while the native scene renderer is switched off.
class RenderModeIndicator final : public rex::ui::ImGuiDialog {
 public:
  explicit RenderModeIndicator(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  bool WantsContinuousRepaint() const override { return false; }
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace skate3
