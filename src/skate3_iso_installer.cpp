#include "skate3_iso_installer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/overlay/install_wizard_overlay.h>
#include <rex/ui/windowed_app_context.h>

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

#if defined(__APPLE__)
std::filesystem::path PickIsoFileMacOS();
#endif

namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr std::array<uint64_t, 6> kPossibleGameOffsets = {
    0x00000000ull, 0x00010000ull, 0x0000FB20ull, 0x00020600ull, 0x02080000ull, 0x0FD90000ull};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::string_view kDefaultXex = "default.xex";

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool IsUnsafeIsoPath(std::string_view path) {
  if (path.empty() || path.starts_with('/') || path.starts_with('\\')) {
    return true;
  }
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    auto component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return true;
    }
    if (end == path.size()) {
      break;
    }
    start = end + 1;
  }
  return false;
}

#if defined(_WIN32)
std::filesystem::path PickIsoFile() {
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Xbox 360 ISO (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select Skate 3 Xbox 360 ISO";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) {
    return {};
  }
  return filename;
}
#elif defined(__APPLE__)
std::filesystem::path PickIsoFile() {
  return skate3::PickIsoFileMacOS();
}
#else
std::filesystem::path PickIsoFile() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Select Skate 3 Xbox 360 ISO", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
  if (!dialog) {
    return {};
  }

  GtkFileFilter* iso_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(iso_filter, "Xbox 360 ISO (*.iso)");
  gtk_file_filter_add_pattern(iso_filter, "*.iso");
  gtk_file_filter_add_pattern(iso_filter, "*.ISO");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), iso_filter);

  GtkFileFilter* all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All files");
  gtk_file_filter_add_pattern(all_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

  std::filesystem::path result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename) {
      result = filename;
      g_free(filename);
    }
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return result;
}
#endif

struct IsoEntry {
  std::string path;
  uint64_t offset = 0;
  uint64_t size = 0;
};

class XboxIsoReader {
 public:
  bool Open(const std::filesystem::path& path, std::string& error) {
    iso_path_ = path;
    file_.open(path, std::ios::binary);
    if (!file_) {
      error = "Unable to open the selected ISO.";
      return false;
    }

    file_.seekg(0, std::ios::end);
    file_size_ = static_cast<uint64_t>(file_.tellg());
    file_.seekg(0, std::ios::beg);

    uint64_t game_offset = 0;
    bool found_magic = false;
    std::array<char, kXdvdfsMagic.size()> magic{};
    for (uint64_t candidate : kPossibleGameOffsets) {
      const uint64_t magic_offset = candidate + 32 * kSectorSize;
      if (magic_offset + magic.size() > file_size_) {
        continue;
      }
      file_.seekg(static_cast<std::streamoff>(magic_offset), std::ios::beg);
      file_.read(magic.data(), static_cast<std::streamsize>(magic.size()));
      if (std::string_view(magic.data(), magic.size()) == kXdvdfsMagic) {
        game_offset = candidate;
        found_magic = true;
        break;
      }
    }

    if (!found_magic) {
      error = "The selected file is not a recognized Xbox 360 game ISO.";
      return false;
    }

    std::array<uint8_t, 8> root_info{};
    const uint64_t root_info_offset = game_offset + 32 * kSectorSize + 20;
    if (!ReadAt(root_info_offset, root_info.data(), root_info.size())) {
      error = "The ISO root directory could not be read.";
      return false;
    }

    const uint32_t root_sector = ReadLe32(root_info.data());
    const uint32_t root_size = ReadLe32(root_info.data() + 4);
    if (root_size < 13 || root_size > 32 * 1024 * 1024) {
      error = "The ISO root directory is invalid.";
      return false;
    }

    entries_.clear();
    if (!ParseDirectory("", game_offset, game_offset + uint64_t(root_sector) * kSectorSize, error)) {
      return false;
    }

    if (!HasFile(kDefaultXex)) {
      error = "The ISO does not contain default.xex.";
      return false;
    }

    return true;
  }

  bool HasFile(std::string_view path) const {
    const auto wanted = ToLower(std::string(path));
    return std::any_of(entries_.begin(), entries_.end(), [&](const IsoEntry& entry) {
      return ToLower(entry.path) == wanted;
    });
  }

  uint64_t TotalSize() const {
    uint64_t total = 0;
    for (const auto& entry : entries_) {
      total += entry.size;
    }
    return total;
  }

  bool ExtractAll(const std::filesystem::path& target_root, std::atomic<uint64_t>& copied_bytes,
                  std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(target_root, ec);
    if (ec) {
      error = "Unable to create the game directory.";
      return false;
    }

    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    for (const auto& entry : entries_) {
      if (IsUnsafeIsoPath(entry.path)) {
        error = "The ISO contains an unsafe file path.";
        return false;
      }

      const auto target = target_root / std::filesystem::path(entry.path);
      std::filesystem::create_directories(target.parent_path(), ec);
      if (ec) {
        error = "Unable to create an install subdirectory.";
        return false;
      }

      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Unable to create " + entry.path + ".";
        return false;
      }

      uint64_t remaining = entry.size;
      uint64_t read_offset = entry.offset;
      while (remaining > 0) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
        if (!ReadAt(read_offset, buffer.data(), chunk)) {
          error = "Failed to read " + entry.path + " from the ISO.";
          return false;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
        if (!out) {
          error = "Failed to write " + entry.path + ".";
          return false;
        }
        remaining -= chunk;
        read_offset += chunk;
        copied_bytes.fetch_add(chunk, std::memory_order_relaxed);
      }
    }

    return true;
  }

 private:
  bool ReadAt(uint64_t offset, void* data, size_t size) {
    if (offset + size > file_size_) {
      return false;
    }
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return file_.good();
  }

  bool ParseDirectory(const std::string& prefix, uint64_t game_offset, uint64_t directory_offset,
                      std::string& error) {
    struct PendingNode {
      uint64_t directory_offset = 0;
      uint32_t node_offset = 0;
      std::string prefix;
    };

    std::vector<PendingNode> pending;
    pending.push_back({directory_offset, 0, prefix});
    std::array<uint8_t, 14> header{};
    size_t visited = 0;

    while (!pending.empty()) {
      auto node = std::move(pending.back());
      pending.pop_back();
      if (++visited > 500000) {
        error = "The ISO directory tree is unexpectedly large.";
        return false;
      }

      const uint64_t entry_offset = node.directory_offset + node.node_offset;
      if (!ReadAt(entry_offset, header.data(), header.size())) {
        error = "Failed to read an ISO directory entry.";
        return false;
      }

      const uint16_t left = ReadLe16(header.data());
      const uint16_t right = ReadLe16(header.data() + 2);
      const uint32_t sector = ReadLe32(header.data() + 4);
      const uint32_t length = ReadLe32(header.data() + 8);
      const uint8_t attributes = header[12];
      const uint8_t name_length = header[13];
      if (name_length == 0 || name_length > 240) {
        error = "The ISO contains an invalid directory entry name.";
        return false;
      }

      std::string name(name_length, '\0');
      if (!ReadAt(entry_offset + header.size(), name.data(), name.size())) {
        error = "Failed to read an ISO directory entry name.";
        return false;
      }

      if (left) {
        pending.push_back({node.directory_offset, static_cast<uint32_t>(left) * 4u, node.prefix});
      }
      if (right) {
        pending.push_back({node.directory_offset, static_cast<uint32_t>(right) * 4u, node.prefix});
      }

      const bool is_directory = (attributes & 0x10) != 0;
      const std::string full_path = node.prefix + name;
      if (is_directory) {
        if (length != 0) {
          pending.push_back({game_offset + uint64_t(sector) * kSectorSize, 0, full_path + "/"});
        }
      } else {
        entries_.push_back({full_path, game_offset + uint64_t(sector) * kSectorSize, length});
      }
    }

    return true;
  }

  std::filesystem::path iso_path_;
  std::ifstream file_;
  uint64_t file_size_ = 0;
  std::vector<IsoEntry> entries_;
};

}  // namespace

bool IsGameInstalled(const std::filesystem::path& game_root) {
  return std::filesystem::is_regular_file(game_root / std::string(kDefaultXex));
}

void ShowRexglueIsoInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                 std::function<void(rex::PathConfig)> complete) {
  auto pick_source = []() { return PickIsoFile(); };
  auto install = [game_root = runtime_paths.game_data_root](
                     const std::filesystem::path& source, std::atomic<uint64_t>& copied_bytes,
                     std::atomic<uint64_t>& total_bytes, std::string& error) {
    XboxIsoReader iso;
    if (!iso.Open(source, error)) {
      return false;
    }
    total_bytes = iso.TotalSize();
    if (!iso.ExtractAll(game_root, copied_bytes, error)) {
      return false;
    }
    if (!IsGameInstalled(game_root)) {
      error = "Installation completed, but default.xex was not found in the install directory.";
      return false;
    }
    return true;
  };

  new rex::ui::InstallWizardDialog(
      drawer, "Skate 3 Setup",
      "Skate 3 game files were not found. Select your Xbox 360 ISO to install them.",
      runtime_paths.game_data_root.string(), std::move(pick_source), std::move(install),
      [runtime_paths = std::move(runtime_paths), complete = std::move(complete)]() mutable {
        if (complete) {
          complete(std::move(runtime_paths));
        }
      });
}

bool RunRexglueIsoInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                        rex::ui::Window* window,
                                        rex::ui::ImGuiDrawer* drawer,
                                        rex::PathConfig runtime_paths,
                                        rex::PathConfig& installed_paths) {
  struct InstallResult {
    bool done = false;
    bool ok = false;
    rex::PathConfig paths;
  };

  auto result = std::make_shared<InstallResult>();
  auto install = [game_root = runtime_paths.game_data_root](
                     const std::filesystem::path& source, std::atomic<uint64_t>& copied_bytes,
                     std::atomic<uint64_t>& total_bytes, std::string& error) {
    XboxIsoReader iso;
    if (!iso.Open(source, error)) {
      return false;
    }
    total_bytes = iso.TotalSize();
    if (!iso.ExtractAll(game_root, copied_bytes, error)) {
      return false;
    }
    if (!IsGameInstalled(game_root)) {
      error = "Installation completed, but default.xex was not found in the install directory.";
      return false;
    }
    return true;
  };

  if (const char* automated_iso = std::getenv("SKATE3_INSTALL_ISO");
      automated_iso != nullptr && *automated_iso != '\0') {
    std::atomic<uint64_t> copied_bytes{0};
    std::atomic<uint64_t> total_bytes{0};
    std::string error;
    REXLOG_INFO("Installing Skate 3 game files from SKATE3_INSTALL_ISO={}", automated_iso);
    if (!install(std::filesystem::path(automated_iso), copied_bytes, total_bytes, error)) {
      REXLOG_ERROR("Automated ISO installation failed: {}", error);
      return false;
    }
    installed_paths = std::move(runtime_paths);
    REXLOG_INFO("Automated ISO installation completed successfully");
    return true;
  }

  ShowRexglueIsoInstallWizard(drawer, runtime_paths,
                              [result](rex::PathConfig runtime_paths) mutable {
                                result->paths = std::move(runtime_paths);
                                result->ok = true;
                                result->done = true;
                              });

#if defined(_WIN32)
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
#endif

  REXLOG_INFO("Entering rexglue ISO installer pump");
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
    REXLOG_INFO("Leaving rexglue ISO installer pump without installation");
    return false;
  }

  installed_paths = std::move(result->paths);
  REXLOG_INFO("Leaving rexglue ISO installer pump after successful installation");
  return true;
}

}  // namespace skate3
