#include "skate3_setup_summary.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <rex/ui/window_win.h>
#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

#include <imgui.h>

namespace skate3 {

namespace {

std::string FormatSize(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buf;
}

class SetupSummaryDialog final : public rex::ui::ImGuiDialog {
 public:
  using FinishCallback = std::function<void()>;

  SetupSummaryDialog(rex::ui::ImGuiDrawer* drawer, SetupSummary summary,
                     FinishCallback on_finish)
      : rex::ui::ImGuiDialog(drawer),
        summary_(std::move(summary)),
        on_finish_(std::move(on_finish)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float width = std::min(700.0f, std::max(420.0f, io.DisplaySize.x - 64.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
    ImGui::PushFont(nullptr, 18.0f);

    if (ImGui::Begin("Skate 3 Setup Complete", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Setup Complete!");
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // Profile
      if (summary_.profile_created) {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Profile");
        ImGui::TextWrapped("Created: %s", summary_.profile_name.c_str());
      } else {
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Profile");
        ImGui::TextWrapped("Loaded existing profile");
      }
      ImGui::Spacing();

      // ISO
      ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Game Files");
      if (summary_.iso_installed) {
        ImGui::TextWrapped("ISO installed: %s", summary_.iso_path.c_str());
      } else {
        ImGui::TextWrapped("ISO already present");
      }
      ImGui::Spacing();

      // Title Update
      ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Title Update");
      if (summary_.tu_installed) {
        ImGui::TextWrapped("Title Update 4 installed");
      } else if (summary_.tu_already_present) {
        ImGui::TextWrapped("Title Update 4 already present");
      }
      ImGui::Spacing();

      // DLC
      ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "DLC Content");
      if (summary_.dlc_skipped) {
        ImGui::TextWrapped("DLC installation skipped");
      } else if (summary_.dlc_packages_installed > 0) {
        ImGui::TextWrapped("Installed %zu package(s) (%s):", summary_.dlc_packages_installed,
                           FormatSize(summary_.dlc_total_bytes).c_str());
        ImGui::BeginChild("##dlc_list", ImVec2(-1, 120), ImGuiChildFlags_Borders);
        for (const auto& name : summary_.dlc_package_names) {
          ImGui::BulletText("%s", name.c_str());
        }
        ImGui::EndChild();
      } else {
        ImGui::TextWrapped("No DLC packages found");
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      if (ImGui::Button("Start Game", ImVec2(200.0f, 42.0f))) {
        Close();
        if (on_finish_) {
          on_finish_();
        }
      }

      ImGui::End();
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(3);
  }

 private:
  SetupSummary summary_;
  FinishCallback on_finish_;
};

}  // namespace

void ShowSetupSummary(rex::ui::ImGuiDrawer* drawer, SetupSummary summary,
                      std::function<void()> on_finish) {
  new SetupSummaryDialog(drawer, std::move(summary), std::move(on_finish));
}

bool RunSetupSummaryBlocking(rex::ui::WindowedAppContext& app_context,
                             rex::ui::Window* window,
                             rex::ui::ImGuiDrawer* drawer,
                             SetupSummary summary) {
  struct FinishResult {
    bool done = false;
  };
  auto result = std::make_shared<FinishResult>();

  ShowSetupSummary(
      drawer, std::move(summary),
      [result]() {
        result->done = true;
      });

#if defined(_WIN32)
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
#endif

  REXLOG_INFO("Entering Skate 3 setup summary pump");
  while (!result->done && !app_context.HasQuitFromUIThread()) {
    app_context.ExecutePendingFunctionsFromUIThread();

#if defined(_WIN32)
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        app_context.QuitFromUIThread();
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (app_context.HasQuitFromUIThread()) {
      break;
    }
    if (window) {
      window->RequestPaint();
    }
    if (hwnd) {
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
#else
    if (window) {
      window->RequestPaint();
    }
#if !defined(__APPLE__)
    while (gtk_events_pending()) {
      gtk_main_iteration_do(FALSE);
    }
#endif
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  REXLOG_INFO("Leaving setup summary pump");
  return result->done;
}

}  // namespace skate3
