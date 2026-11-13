// Application icon from the user's own game files, see skate3_win_icon.h.

#include "skate3_win_icon.h"

#if defined(_WIN32)

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace skate3 {
namespace {

// First embedded PNG in the nxeart dashboard-art package = the 64x64 title
// tile (two identical copies exist; either is fine).
std::vector<uint8_t> ExtractFirstPng(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return {};
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  static const uint8_t kMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  static const uint8_t kIend[4] = {'I', 'E', 'N', 'D'};
  for (size_t i = 0; i + 8 <= data.size(); ++i) {
    if (std::memcmp(data.data() + i, kMagic, 8) != 0) {
      continue;
    }
    for (size_t j = i + 8; j + 8 <= data.size(); ++j) {
      if (std::memcmp(data.data() + j, kIend, 4) == 0) {
        // IEND chunk data end + 4-byte CRC.
        return std::vector<uint8_t>(data.begin() + i, data.begin() + j + 8);
      }
    }
    break;
  }
  return {};
}

}  // namespace

void ApplyGameIconFromGameData(const std::filesystem::path& game_data_root,
                               void* native_window_handle) {
  std::filesystem::path nxeart = game_data_root / "nxeart";
  std::error_code ec;
  if (!std::filesystem::exists(nxeart, ec)) {
    nxeart = game_data_root / "game" / "nxeart";
  }
  HWND hwnd = static_cast<HWND>(native_window_handle);
  std::thread([nxeart, hwnd] {
    const std::vector<uint8_t> png = ExtractFirstPng(nxeart);
    if (png.empty()) {
      REXLOG_INFO("win-icon: no title icon found in {}", nxeart.string());
      return;
    }
    if (hwnd != nullptr) {
      // CreateIconFromResourceEx accepts PNG image data (Vista+). Explicit
      // small size for crisp titlebar/taskbar small renditions.
      HICON icon_big = CreateIconFromResourceEx(
          const_cast<PBYTE>(png.data()), DWORD(png.size()), TRUE, 0x00030000,
          0, 0, LR_DEFAULTCOLOR);
      HICON icon_small = CreateIconFromResourceEx(
          const_cast<PBYTE>(png.data()), DWORD(png.size()), TRUE, 0x00030000,
          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
          LR_DEFAULTCOLOR);
      DWORD_PTR unused = 0;
      if (icon_big) {
        SendMessageTimeoutW(hwnd, WM_SETICON, ICON_BIG,
                            reinterpret_cast<LPARAM>(icon_big), SMTO_NORMAL,
                            2000, &unused);
      }
      if (icon_small) {
        SendMessageTimeoutW(hwnd, WM_SETICON, ICON_SMALL,
                            reinterpret_cast<LPARAM>(icon_small), SMTO_NORMAL,
                            2000, &unused);
      }
      // HICONs stay alive for the window's lifetime (never destroyed:
      // process-lifetime singletons).
    }
  }).detach();
}

}  // namespace skate3

#else  // !_WIN32

namespace skate3 {
void ApplyGameIconFromGameData(const std::filesystem::path&, void*) {}
}  // namespace skate3

#endif
