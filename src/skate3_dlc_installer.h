#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace skate3 {

void ShowDlcInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                          std::function<void(rex::PathConfig)> complete,
                          std::function<void()> quit_callback);
bool RunDlcInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                 rex::ui::Window* window,
                                 rex::ui::ImGuiDrawer* drawer,
                                 rex::PathConfig runtime_paths,
                                 rex::PathConfig& installed_paths);

}  // namespace skate3
