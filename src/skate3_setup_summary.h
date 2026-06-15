#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>

namespace skate3 {

struct SetupSummary {
  bool profile_created = false;
  std::string profile_name;
  bool iso_installed = false;
  std::string iso_path;
  bool tu_installed = false;
  bool tu_already_present = false;
  size_t dlc_packages_installed = 0;
  uint64_t dlc_total_bytes = 0;
  std::vector<std::string> dlc_package_names;
  bool dlc_skipped = false;
};

void ShowSetupSummary(rex::ui::ImGuiDrawer* drawer, SetupSummary summary,
                      std::function<void()> on_finish);

bool RunSetupSummaryBlocking(rex::ui::WindowedAppContext& app_context,
                             rex::ui::Window* window,
                             rex::ui::ImGuiDrawer* drawer,
                             SetupSummary summary);

}  // namespace skate3
