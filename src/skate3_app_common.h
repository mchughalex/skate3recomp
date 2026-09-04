#pragma once

#include "generated/skate3_init.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <rex/rex_app.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include "skate3_native_debug_dialog.h"

namespace rex::ui {
class ImGuiDrawer;
}

class Skate3BaseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Skate3BaseApp() override;

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  void InstallRecipeOverlay();
  void InstallBigDeviceAliases();
  void InstallDlcPackages();
  void ToggleSimpleSettings();
  void ToggleNativeDebug();
  void TogglePerformanceWindow();
  void ApplySettingsCursorMode();
  void ApplyGameplayCursorMode();
  void RestartGame();
  void SaveDrawFingerprintLog();
  void LogUserMarker();
  void LogDebugMarker();
  void ApplySelectedProfileToRuntime();

  static bool IsRecipeNameChar(char c);
  static std::set<std::string> DiscoverRecipeAliases(
      const std::filesystem::path& content_root);
  static bool CreateOverlayDirectory(const std::filesystem::path& overlay_root,
                                     std::string_view guest_path);

  std::filesystem::path config_path_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path profiles_path_;
  std::unique_ptr<rex::ui::SimpleSettingsDialog> simple_settings_dialog_;
  std::unique_ptr<skate3::NativeDebugDialog> native_debug_dialog_;
  std::unique_ptr<skate3::PerformanceDialog> performance_dialog_;
  std::unique_ptr<skate3::RenderModeIndicator> render_mode_indicator_;
  bool recipe_overlay_installed_ = false;
  bool big_device_aliases_installed_ = false;
  std::atomic<uint32_t> debug_marker_count_{0};
};
