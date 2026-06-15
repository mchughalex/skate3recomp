#include "skate3_dlc_installer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/filesystem/devices/stfs_container_device.h>

#include <imgui.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <commdlg.h>
#include <windows.h>
#include <rex/ui/window_win.h>
#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

namespace skate3 {

namespace {

constexpr std::string_view kDlcDirectoryName = "dlc";

bool LooksLikeStfsPackage(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return false;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size < 4 || size > 2ull * 1024 * 1024 * 1024) {
    return false;
  }
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return false;
  }
  char magic[4] = {};
  const bool ok = std::fread(magic, 1, 4, f) == 4;
  std::fclose(f);
  if (!ok) {
    return false;
  }
  return (std::string_view(magic, 4) == "CON ") || (std::string_view(magic, 4) == "LIVE") ||
         (std::string_view(magic, 4) == "PIRS");
}

struct DlcPackage {
  std::filesystem::path path;
  std::string filename;
  std::string display_name;
  uint64_t size = 0;
};

std::string Utf16ToUtf8(const std::u16string& u16) {
  std::string result;
  result.reserve(u16.size());
  for (char16_t ch : u16) {
    if (ch < 0x80) {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('?');
    }
  }
  return result;
}

std::vector<DlcPackage> ScanForDlcPackages(const std::filesystem::path& dir) {
  std::vector<DlcPackage> packages;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!LooksLikeStfsPackage(entry.path())) {
      continue;
    }

    DlcPackage pkg;
    pkg.path = entry.path();
    pkg.filename = entry.path().filename().string();
    pkg.size = std::filesystem::file_size(entry.path(), ec);

    // Try to read display name from STFS header
    auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(entry.path());
    if (header) {
      auto name = header->metadata.display_name(rex::filesystem::XLanguage::kEnglish);
      if (!name.empty()) {
        pkg.display_name = Utf16ToUtf8(name);
      }
      if (pkg.display_name.empty()) {
        name = header->metadata.title_name();
        if (!name.empty()) {
          pkg.display_name = Utf16ToUtf8(name);
        }
      }
    }
    if (pkg.display_name.empty()) {
      pkg.display_name = pkg.filename;
    }

    packages.push_back(std::move(pkg));
  }
  std::sort(packages.begin(), packages.end(),
            [](const auto& a, const auto& b) { return a.display_name < b.display_name; });
  return packages;
}

bool CopyDlcPackages(const std::vector<DlcPackage>& packages,
                     const std::filesystem::path& dest_dir, std::atomic<uint64_t>& copied_bytes,
                     std::atomic<uint64_t>& total_bytes, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  if (ec) {
    error = "Unable to create " + dest_dir.string() + ".";
    return false;
  }

  total_bytes.store(0, std::memory_order_relaxed);
  for (const auto& pkg : packages) {
    total_bytes.fetch_add(pkg.size, std::memory_order_relaxed);
  }

  for (const auto& pkg : packages) {
    const auto dest = dest_dir / pkg.filename;
    std::ifstream in(pkg.path, std::ios::binary);
    if (!in) {
      error = "Unable to open " + pkg.path.string() + ".";
      return false;
    }
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Unable to create " + dest.string() + ".";
      return false;
    }
    std::vector<char> buf(64 * 1024);
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const auto count = in.gcount();
      if (count > 0) {
        out.write(buf.data(), count);
        copied_bytes.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
      }
    }
    if (!out) {
      error = "Failed to write " + dest.string() + ".";
      return false;
    }
  }
  return true;
}

std::string FormatSize(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buf;
}

#if defined(_WIN32)
std::filesystem::path PickDlcFolder() {
  wchar_t folder[MAX_PATH] = {};
  BROWSEINFOW bi{};
  bi.hwndOwner = GetActiveWindow();
  bi.lpszTitle = L"Select the folder containing Skate 3 DLC packages";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return {};
  }
  bool ok = SHGetPathFromIDListW(pidl, folder);
  CoTaskMemFree(pidl);
  return ok ? folder : std::filesystem::path{};
}
#elif defined(__APPLE__)
std::filesystem::path PickDlcFolder();
#else
std::filesystem::path PickDlcFolder() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Select the folder containing Skate 3 DLC packages", nullptr,
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel", GTK_RESPONSE_CANCEL, "_Open",
      GTK_RESPONSE_ACCEPT, nullptr);
  if (!dialog) {
    return {};
  }

  std::filesystem::path result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (folder) {
      result = folder;
      g_free(folder);
    }
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return result;
}
#endif

// ----------------------------------------------------------------------------
// Custom DLC install dialog
// ----------------------------------------------------------------------------

class DlcInstallDialog final : public rex::ui::ImGuiDialog {
 public:
  using CompleteCallback = std::function<void()>;
  using QuitCallback = std::function<void()>;

  DlcInstallDialog(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                   CompleteCallback complete, QuitCallback quit_callback)
      : rex::ui::ImGuiDialog(drawer),
        runtime_paths_(std::move(runtime_paths)),
        game_root_(runtime_paths_.game_data_root),
        dest_dir_(game_root_ / std::string(kDlcDirectoryName)),
        complete_(std::move(complete)),
        quit_callback_(std::move(quit_callback)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float width = std::min(760.0f, std::max(460.0f, io.DisplaySize.x - 64.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
    ImGui::PushFont(nullptr, 18.0f);

    if (ImGui::Begin("Skate 3 DLC Content", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      switch (state_) {
        case State::kSelectFolder:
          OnDrawSelectFolder();
          break;
        case State::kShowPackages:
          OnDrawShowPackages();
          break;
        case State::kInstalling:
          OnDrawInstalling();
          break;
        case State::kDone:
          OnDrawDone();
          break;
        case State::kFailed:
          OnDrawFailed();
          break;
      }

      ImGui::End();
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(3);
  }

 private:
  enum class State {
    kSelectFolder,
    kShowPackages,
    kInstalling,
    kDone,
    kFailed,
  };

  void OnDrawSelectFolder() {
    ImGui::TextWrapped(
        "Optionally install downloadable content (skateboards, costumes, parks, etc.).");
    ImGui::Spacing();
    ImGui::TextWrapped("Select the folder containing your legally obtained DLC package files.");
    ImGui::Spacing();
    ImGui::TextWrapped("Install directory: %s", dest_dir_.string().c_str());
    ImGui::Spacing();

    if (ImGui::Button("Select DLC folder...", ImVec2(220.0f, 42.0f))) {
      auto folder = PickDlcFolder();
      if (!folder.empty()) {
        source_dir_ = folder;
        packages_ = ScanForDlcPackages(folder);
        if (packages_.empty()) {
          error_ = "No Xbox 360 DLC packages (CON/LIVE/PIRS) found in " + folder.string() + ".";
          state_ = State::kFailed;
        } else {
          state_ = State::kShowPackages;
        }
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(120.0f, 42.0f))) {
      WriteSkipMarker();
      Close();
      if (complete_) {
        complete_();
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 42.0f))) {
      Close();
    }
  }

  void OnDrawShowPackages() {
    ImGui::TextWrapped("Found %zu DLC package(s) in:", packages_.size());
    ImGui::TextWrapped("%s", source_dir_.string().c_str());
    ImGui::Spacing();

    ImGui::BeginChild("##packages", ImVec2(-1, 200), ImGuiChildFlags_Borders);
    uint64_t total_size = 0;
    for (const auto& pkg : packages_) {
      ImGui::Text("%s  (%s)", pkg.display_name.c_str(), FormatSize(pkg.size).c_str());
      total_size += pkg.size;
    }
    ImGui::EndChild();

    ImGui::TextWrapped("Total: %s", FormatSize(total_size).c_str());
    ImGui::Spacing();

    if (ImGui::Button("Install", ImVec2(160.0f, 42.0f))) {
      StartInstall();
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(120.0f, 42.0f))) {
      WriteSkipMarker();
      Close();
      if (complete_) {
        complete_();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 42.0f))) {
      Close();
    }
  }

  void OnDrawInstalling() {
    ImGui::TextWrapped("Copying DLC packages...");
    ImGui::Spacing();

    const uint64_t total = total_bytes_.load(std::memory_order_relaxed);
    const uint64_t copied = copied_bytes_.load(std::memory_order_relaxed);
    const float progress =
        total == 0 ? 0.0f : std::clamp(static_cast<float>(double(copied) / double(total)), 0.0f, 1.0f);
    ImGui::ProgressBar(progress, ImVec2(-1.0f, 28.0f));

    ImGui::Spacing();
    ImGui::TextWrapped("%s / %s", FormatSize(copied).c_str(), FormatSize(total).c_str());

    FinishInstallIfNeeded();
  }

  void OnDrawDone() {
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "DLC packages installed successfully!");
    ImGui::Spacing();
    ImGui::TextWrapped("Installed %zu package(s) to:", packages_.size());
    ImGui::TextWrapped("%s", dest_dir_.string().c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Please close and reopen the game for the DLC to take effect.");
    ImGui::Spacing();

    if (ImGui::Button("Finish", ImVec2(160.0f, 42.0f))) {
      Close();
      if (quit_callback_) {
        quit_callback_();
      }
    }
  }

  void OnDrawFailed() {
    ImGui::TextColored(ImVec4(0.95f, 0.28f, 0.24f, 1.0f), "%s", error_.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Try Again", ImVec2(160.0f, 42.0f))) {
      state_ = State::kSelectFolder;
      error_.clear();
      packages_.clear();
      source_dir_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(120.0f, 42.0f))) {
      WriteSkipMarker();
      Close();
      if (complete_) {
        complete_();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 42.0f))) {
      Close();
    }
  }

  void StartInstall() {
    if (install_thread_.joinable()) {
      install_thread_.join();
    }
    copied_bytes_ = 0;
    total_bytes_ = 0;
    install_done_ = false;
    install_ok_ = false;
    error_.clear();
    state_ = State::kInstalling;

    install_thread_ = std::thread([this]() {
      std::string error;
      const bool ok = CopyDlcPackages(packages_, dest_dir_, copied_bytes_, total_bytes_, error);
      error_ = std::move(error);
      install_ok_ = ok;
      install_done_ = true;
    });
  }

  void FinishInstallIfNeeded() {
    if (state_ != State::kInstalling || !install_done_.load(std::memory_order_acquire)) {
      return;
    }
    if (install_thread_.joinable()) {
      install_thread_.join();
    }
    if (install_ok_.load(std::memory_order_acquire)) {
      state_ = State::kDone;
      REXLOG_INFO("Skate 3 DLC packages installed: {} files to {}", packages_.size(),
                  dest_dir_.string());
    } else {
      state_ = State::kFailed;
    }
  }

  void WriteSkipMarker() {
    std::error_code ec;
    std::filesystem::create_directories(dest_dir_, ec);
    std::ofstream out(dest_dir_ / ".skip");
  }

  rex::PathConfig runtime_paths_;
  std::filesystem::path game_root_;
  std::filesystem::path dest_dir_;
  CompleteCallback complete_;
  QuitCallback quit_callback_;

  State state_ = State::kSelectFolder;
  std::filesystem::path source_dir_;
  std::vector<DlcPackage> packages_;
  std::string error_;

  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  std::atomic<bool> install_ok_{false};
  std::atomic<uint64_t> copied_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
};

}  // namespace

void ShowDlcInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                          std::function<void(rex::PathConfig)> complete,
                          std::function<void()> quit_callback) {
  auto wrapped_complete = [complete = std::move(complete), runtime_paths]() mutable {
    if (complete) {
      complete(runtime_paths);
    }
  };
  new DlcInstallDialog(drawer, std::move(runtime_paths), std::move(wrapped_complete),
                       std::move(quit_callback));
}

bool RunDlcInstallWizardBlocking(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                                 rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                 rex::PathConfig& installed_paths) {
  struct InstallResult {
    bool done = false;
    bool ok = false;
    rex::PathConfig paths;
  };
  auto result = std::make_shared<InstallResult>();

  ShowDlcInstallWizard(
      drawer, runtime_paths,
      [result](rex::PathConfig runtime_paths) mutable {
        result->paths = std::move(runtime_paths);
        result->ok = true;
        result->done = true;
      },
      [&app_context]() { app_context.QuitFromUIThread(); });

#if defined(_WIN32)
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
#endif

  REXLOG_INFO("Entering Skate 3 DLC installer pump");
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

  if (!result->ok) {
    REXLOG_INFO("Leaving DLC installer pump (user cancelled or app quitting)");
    return false;
  }

  installed_paths = std::move(result->paths);
  REXLOG_INFO("Leaving DLC installer pump after successful installation");
  return true;
}

}  // namespace skate3
