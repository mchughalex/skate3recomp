#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace skate3 {

bool IsGameInstalled(const std::filesystem::path& game_root);
// Windowless setup: extracts the game files from an Xbox 360 ISO into game_root,
// printing "progress <0-1> <text>" lines on stdout. Returns 0 on success, or 1
// after printing one line on stderr.
int RunIsoPrepare(const std::filesystem::path& source, const std::filesystem::path& game_root);
void ShowRexglueIsoInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                 std::function<void(rex::PathConfig)> complete);
bool RunRexglueIsoInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                        rex::ui::Window* window,
                                        rex::ui::ImGuiDrawer* drawer,
                                        rex::PathConfig runtime_paths,
                                        rex::PathConfig& installed_paths);

}  // namespace skate3
