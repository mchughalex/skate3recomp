#include "skate3_title_update_installer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/acquire_wizard_overlay.h>
#include <rex/ui/windowed_app_context.h>

#include "third_party/rexglue-sdk/thirdparty/crypto/sha256.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <commdlg.h>
#include <windows.h>

#include <winhttp.h>

#include <rex/ui/window_win.h>
#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

REXCVAR_DEFINE_STRING(skate3_title_update_url,
                      "https://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid=21774",
                      "Skate 3",
                      "Download URL for the Skate 3 Title Update 3 package (TU3)");

namespace skate3 {

#if defined(__APPLE__)
std::filesystem::path PickTitleUpdateFileMacOS();
#endif

namespace {

// The recompilation is generated against exactly these payloads
// (Skate 3 title 454108E6, base 3.0.0.0 -> 3.0.3.0, media ID 5C087C2C).
struct TitleUpdatePayload {
  std::string_view container_path;  // path inside the TU STFS package
  std::string_view staged_path;     // path relative to the game root
  uint64_t size;
  std::string_view sha256;  // lowercase hex
};

constexpr std::array<TitleUpdatePayload, 2> kPayloads = {{
    {"default.xexp", "default.xexp", 1787904,
     "048550f7961000009b9f9e340b919b64db193accb7e51a1f2c37f24649e365f5"},
    {"data/webkit/EAWebkit.xexp", "data/webkit/EAWebkit.xexp", 4096,
     "5eb1090013bc5eb8a2ef5d6d8655e640fd26c1c3a4916a5541d0d7ce331990b1"},
}};

// Size of the known TU4 STFS container; used as the progress estimate when the
// HTTP response carries no Content-Length, and as a sanity cap for downloads.
constexpr uint64_t kContainerSize = 1859584;
constexpr uint64_t kMaxPackageSize = 256ull * 1024 * 1024;

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Sha256OfData(const uint8_t* data, size_t size) {
  sha256::SHA256 hasher;
  return hasher(data, size);
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out,
                   std::string& error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = "Unable to read " + path.string() + ".";
    return false;
  }
  if (size > kMaxPackageSize) {
    error = "The selected file is too large to be a Skate 3 title update.";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Unable to open " + path.string() + ".";
    return false;
  }
  out.resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!file) {
    error = "Failed to read " + path.string() + ".";
    return false;
  }
  return true;
}

const TitleUpdatePayload* MatchPayload(const std::vector<uint8_t>& data) {
  for (const auto& payload : kPayloads) {
    if (data.size() == payload.size && Sha256OfData(data.data(), data.size()) == payload.sha256) {
      return &payload;
    }
  }
  return nullptr;
}

bool StagePayload(const TitleUpdatePayload& payload, const std::vector<uint8_t>& data,
                  const std::filesystem::path& game_root, std::string& error) {
  const auto target = game_root / std::filesystem::path(std::string(payload.staged_path));
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) {
    error = "Unable to create " + target.parent_path().string() + ".";
    return false;
  }
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "Unable to create " + target.string() + ".";
    return false;
  }
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) {
    error = "Failed to write " + target.string() + ".";
    return false;
  }
  REXLOG_INFO("Staged Skate 3 title update payload {}", target.string());
  return true;
}

// ----------------------------------------------------------------------------
// Minimal read-only STFS (CON/LIVE/PIRS) package reader, enough to pull the
// xexp payloads out of a title update package.
// ----------------------------------------------------------------------------

uint16_t Be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t Le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U24Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

class StfsPackageReader {
 public:
  static constexpr uint32_t kBlockSize = 0x1000;
  static constexpr uint32_t kEndOfChain = 0xFFFFFF;

  struct Entry {
    std::string path;
    bool is_dir = false;
    uint32_t start_block = 0;
    uint32_t length = 0;
  };

  static bool LooksLikeStfs(const std::vector<uint8_t>& data) {
    if (data.size() < 4) {
      return false;
    }
    const std::string_view magic(reinterpret_cast<const char*>(data.data()), 4);
    return magic == "CON " || magic == "LIVE" || magic == "PIRS";
  }

  bool Open(std::vector<uint8_t> data, std::string& error) {
    data_ = std::move(data);
    if (data_.size() < 0x400 || !LooksLikeStfs(data_)) {
      error = "The file is not an Xbox 360 content package.";
      return false;
    }
    header_size_ = Be32(at(0x340, 4));
    metadata_offset_ = 0x344;
    volume_descriptor_offset_ = metadata_offset_ + 0x35;
    const uint32_t volume_type = Be32(at(metadata_offset_ + 0x65, 4));
    if (volume_type != 0) {
      error = "The content package is not an STFS volume.";
      return false;
    }
    const uint8_t flags = *at(volume_descriptor_offset_ + 2, 1);
    blocks_per_hash_table_ = (flags & 1) ? 1 : 2;
    return true;
  }

  bool ListEntries(std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    const uint8_t* descriptor = at(volume_descriptor_offset_, 8);
    if (!descriptor) {
      error = "The content package header is truncated.";
      return false;
    }
    uint16_t table_block_count = Le16(descriptor + 3);
    uint32_t table_block = U24Le(descriptor + 5);

    for (uint32_t table = 0; table < table_block_count; ++table) {
      const uint64_t table_offset = BlockToOffset(table_block);
      for (uint32_t index = 0; index < 0x40; ++index) {
        const uint8_t* entry = at(table_offset + index * 0x40, 0x40);
        if (!entry) {
          error = "The content package file table is truncated.";
          return false;
        }
        if (entry[0] == 0) {
          break;
        }
        const uint8_t flags = entry[40];
        const uint32_t name_length = flags & 0x3F;
        if (name_length == 0 || name_length > 40) {
          error = "The content package contains an invalid file name.";
          return false;
        }
        Entry parsed;
        std::string name(reinterpret_cast<const char*>(entry), name_length);
        parsed.is_dir = (flags & 0x80) != 0;
        parsed.start_block = U24Le(entry + 47);
        const uint16_t parent_index = Be16(entry + 50);
        parsed.length = Be32(entry + 52);
        std::string parent_path;
        if (parent_index != 0xFFFF) {
          if (parent_index >= entries.size()) {
            error = "The content package directory tree is invalid.";
            return false;
          }
          parent_path = entries[parent_index].path;
        }
        parsed.path = parent_path.empty() ? name : parent_path + "/" + name;
        entries.push_back(std::move(parsed));
      }

      const uint32_t next = NextBlock(table_block);
      if (next == kEndOfChain) {
        break;
      }
      table_block = next;
    }
    return true;
  }

  bool ReadFile(const Entry& entry, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    out.reserve(entry.length);
    uint32_t block_index = entry.start_block;
    uint64_t remaining = entry.length;
    while (remaining > 0 && block_index != kEndOfChain) {
      const uint64_t chunk = std::min<uint64_t>(kBlockSize, remaining);
      const uint8_t* block = at(BlockToOffset(block_index), chunk);
      if (!block) {
        error = "The content package data is truncated.";
        return false;
      }
      out.insert(out.end(), block, block + chunk);
      remaining -= chunk;
      block_index = NextBlock(block_index);
    }
    if (remaining > 0) {
      error = "The content package block chain ended unexpectedly.";
      return false;
    }
    return true;
  }

 private:
  const uint8_t* at(uint64_t offset, uint64_t size) const {
    if (offset + size > data_.size()) {
      return nullptr;
    }
    return data_.data() + offset;
  }

  uint64_t RoundUp(uint64_t value, uint64_t alignment) const {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  uint64_t BlockToOffset(uint32_t block_index) const {
    uint64_t block = block_index;
    uint64_t base = 170;
    for (int i = 0; i < 3; ++i) {
      block += ((block_index + base) / base) * blocks_per_hash_table_;
      if (block_index < base) {
        break;
      }
      base *= 170;
    }
    return RoundUp(header_size_, kBlockSize) + (block << 12);
  }

  uint32_t HashBlockNumber(uint32_t block_index, int hash_level) const {
    const uint32_t block_step0 = 170 + blocks_per_hash_table_;
    const uint32_t block_step1 = 28900 + (170 + 1) * blocks_per_hash_table_;
    if (hash_level == 0) {
      if (block_index < 170) {
        return 0;
      }
      uint32_t block = (block_index / 170) * block_step0;
      block += ((block_index / 28900) + 1) * blocks_per_hash_table_;
      if (block_index < 28900) {
        return block;
      }
      return block + blocks_per_hash_table_;
    }
    if (hash_level == 1) {
      if (block_index < 28900) {
        return block_step0;
      }
      const uint32_t block = (block_index / 28900) * block_step1;
      return block + blocks_per_hash_table_;
    }
    return block_step1;
  }

  uint64_t HashOffset(uint32_t block_index, int hash_level) const {
    return RoundUp(header_size_, kBlockSize) +
           (static_cast<uint64_t>(HashBlockNumber(block_index, hash_level)) << 12);
  }

  uint32_t NextBlock(uint32_t block_index) const {
    const uint64_t hash_offset = HashOffset(block_index, 0);
    const uint8_t* hash_entry = at(hash_offset + (block_index % 170) * 0x18, 0x18);
    if (!hash_entry) {
      return kEndOfChain;
    }
    return Be32(hash_entry + 0x14) & 0xFFFFFF;
  }

  std::vector<uint8_t> data_;
  uint32_t header_size_ = 0;
  uint32_t metadata_offset_ = 0;
  uint32_t volume_descriptor_offset_ = 0;
  uint32_t blocks_per_hash_table_ = 2;
};

// ----------------------------------------------------------------------------
// Download
// ----------------------------------------------------------------------------

#if defined(_WIN32)

std::wstring Widen(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                      length);
  return wide;
}

bool DownloadToFile(const std::string& url, const std::filesystem::path& destination,
                    std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                    std::string& error) {
  std::wstring wide_url = Widen(url);
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    error = "The configured title update URL is invalid.";
    return false;
  }
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.lpszExtraInfo && components.dwExtraInfoLength) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = WinHttpOpen(L"skate3-recomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = "Unable to initialize the download (WinHttpOpen failed).";
    return false;
  }

  // Many hosts (xboxunity.net included) publish both A and AAAA records. Unlike
  // browsers, WinHTTP connects to addresses sequentially, so on a machine with
  // broken IPv6 routing it stalls on the dead IPv6 connect for ~15s before
  // falling back to IPv4. Enable "fast fallback" (Happy Eyeballs, RFC 6555) so
  // WinHTTP races both families in parallel and uses whichever connects first.
#ifndef WINHTTP_OPTION_IPV6_FAST_FALLBACK
#define WINHTTP_OPTION_IPV6_FAST_FALLBACK 140
#endif
  BOOL fast_fallback = TRUE;
  WinHttpSetOption(session, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &fast_fallback,
                   sizeof(fast_fallback));
  // Backstop for OS versions without fast fallback: bound the connect attempt so
  // a dead address family cannot hang the installer indefinitely.
  WinHttpSetTimeouts(session, /*resolve=*/10000, /*connect=*/10000, /*send=*/30000,
                     /*receive=*/60000);
  auto fail = [&](const char* what) {
    error = std::string("Download failed (") + what + ", error " +
            std::to_string(GetLastError()) + "). Check your internet connection.";
    return false;
  };

  bool ok = false;
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  std::ofstream out;
  do {
    connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
      fail("connect");
      break;
    }
    request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
      fail("open request");
      break;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
      fail("request");
      break;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status_code != 200) {
      error = "The download server returned HTTP " + std::to_string(status_code) + ".";
      break;
    }

    DWORD content_length = 0;
    DWORD length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                            WINHTTP_NO_HEADER_INDEX) &&
        content_length > 0) {
      total_bytes.store(content_length, std::memory_order_relaxed);
    }

    out.open(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Unable to create the download file at " + destination.string() + ".";
      break;
    }

    std::vector<char> buffer(64 * 1024);
    uint64_t received = 0;
    bool read_failed = false;
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) {
        fail("read");
        read_failed = true;
        break;
      }
      if (available == 0) {
        break;
      }
      while (available > 0) {
        const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), chunk, &read) || read == 0) {
          fail("read");
          read_failed = true;
          break;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(read));
        received += read;
        available -= read;
        copied_bytes.fetch_add(read, std::memory_order_relaxed);
        if (received > kMaxPackageSize) {
          error = "The download is unexpectedly large; aborting.";
          read_failed = true;
          break;
        }
      }
      if (read_failed) {
        break;
      }
    }
    if (read_failed) {
      break;
    }
    out.flush();
    if (!out) {
      error = "Failed to write the downloaded file.";
      break;
    }
    ok = received > 0;
    if (!ok) {
      error = "The download was empty.";
    }
  } while (false);

  if (request) {
    WinHttpCloseHandle(request);
  }
  if (connection) {
    WinHttpCloseHandle(connection);
  }
  WinHttpCloseHandle(session);
  return ok;
}

#else

bool DownloadToFile(const std::string& url, const std::filesystem::path& destination,
                    std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                    std::string& error) {
  if (url.find('\'') != std::string::npos || url.find('\\') != std::string::npos) {
    error = "The configured title update URL is invalid.";
    return false;
  }
  const std::string destination_str = destination.string();
  if (destination_str.find('\'') != std::string::npos) {
    error = "The temporary download path is not usable.";
    return false;
  }
  const std::string command = "curl -fsSL --connect-timeout 30 --max-time 600 -o '" +
                              destination_str + "' '" + url + "'";
  REXLOG_INFO("Downloading Skate 3 title update via curl");
  const int status = std::system(command.c_str());
  if (status != 0) {
    error = "The download failed (curl exit status " + std::to_string(status) +
            "). Check your internet connection, or select the title update file manually.";
    return false;
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(destination, ec);
  if (ec || size == 0) {
    error = "The download produced no data.";
    return false;
  }
  total_bytes.store(size, std::memory_order_relaxed);
  copied_bytes.store(size, std::memory_order_relaxed);
  return true;
}

#endif  // defined(_WIN32)

// ----------------------------------------------------------------------------
// File picker
// ----------------------------------------------------------------------------

#if defined(_WIN32)
std::filesystem::path PickTitleUpdateFile() {
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Title update package (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select the Skate 3 Title Update 3 package";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) {
    return {};
  }
  return filename;
}
#elif defined(__APPLE__)
std::filesystem::path PickTitleUpdateFile() {
  return skate3::PickTitleUpdateFileMacOS();
}
#else
std::filesystem::path PickTitleUpdateFile() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Select the Skate 3 Title Update 3 package", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
  if (!dialog) {
    return {};
  }

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

bool DownloadAndStageTitleUpdate(const std::filesystem::path& game_root,
                                 std::atomic<uint64_t>& copied_bytes,
                                 std::atomic<uint64_t>& total_bytes, std::string& error) {
  total_bytes.store(kContainerSize, std::memory_order_relaxed);

  std::error_code ec;
  auto temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    temp_dir = game_root;
  }
  const auto temp_file = temp_dir / "skate3_title_update_download.tmp";

  const std::string url = REXCVAR_GET(skate3_title_update_url);
  REXLOG_INFO("Downloading Skate 3 title update from {}", url);
  const bool downloaded = DownloadToFile(url, temp_file, copied_bytes, total_bytes, error);
  bool staged = false;
  if (downloaded) {
    staged = StageTitleUpdateFromFile(temp_file, game_root, error);
  }
  std::filesystem::remove(temp_file, ec);
  return downloaded && staged;
}

}  // namespace

bool IsTitleUpdateInstalled(const std::filesystem::path& game_root) {
  for (const auto& payload : kPayloads) {
    const auto staged = game_root / std::filesystem::path(std::string(payload.staged_path));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(staged, ec) ||
        std::filesystem::file_size(staged, ec) != payload.size || ec) {
      return false;
    }
    std::vector<uint8_t> data;
    std::string error;
    if (!ReadWholeFile(staged, data, error) ||
        Sha256OfData(data.data(), data.size()) != payload.sha256) {
      return false;
    }
  }
  return true;
}

bool StageTitleUpdateFromFile(const std::filesystem::path& source,
                              const std::filesystem::path& game_root, std::string& error) {
  std::vector<uint8_t> data;
  if (!ReadWholeFile(source, data, error)) {
    return false;
  }

  if (StfsPackageReader::LooksLikeStfs(data)) {
    StfsPackageReader package;
    if (!package.Open(std::move(data), error)) {
      return false;
    }
    std::vector<StfsPackageReader::Entry> entries;
    if (!package.ListEntries(entries, error)) {
      return false;
    }
    size_t staged_count = 0;
    for (const auto& payload : kPayloads) {
      const auto wanted = ToLowerCopy(std::string(payload.container_path));
      const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return !entry.is_dir && ToLowerCopy(entry.path) == wanted;
      });
      if (it == entries.end()) {
        error = "The package does not contain " + std::string(payload.container_path) +
                "; it is not the Skate 3 title update.";
        return false;
      }
      std::vector<uint8_t> file_data;
      if (!package.ReadFile(*it, file_data, error)) {
        return false;
      }
      if (file_data.size() != payload.size ||
          Sha256OfData(file_data.data(), file_data.size()) != payload.sha256) {
        error = "The package contains a different version of " +
                std::string(payload.container_path) +
                "; this build requires Title Update 4 (3.0.4.0).";
        return false;
      }
      if (!StagePayload(payload, file_data, game_root, error)) {
        return false;
      }
      ++staged_count;
    }
    return staged_count == kPayloads.size();
  }

  // Not an STFS container; accept a raw xexp payload that matches one of the
  // expected files exactly.
  if (const auto* payload = MatchPayload(data)) {
    if (!StagePayload(*payload, data, game_root, error)) {
      return false;
    }
    if (!IsTitleUpdateInstalled(game_root)) {
      error = std::string(payload->staged_path) +
              " was installed, but the other title update file is still missing. Provide the "
              "full title update package.";
      return false;
    }
    return true;
  }

  error =
      "The selected file is neither the Skate 3 title update package nor one of its "
      "xexp payloads.";
  return false;
}

void ShowTitleUpdateInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                  std::function<void(rex::PathConfig)> complete) {
  const auto game_root = runtime_paths.game_data_root;

  rex::ui::AcquireWizardDialog::Options options;
  options.title = "Skate 3 Title Update";
  options.intro =
      "This build of Skate 3 requires Title Update 3, a free update originally published on "
      "Xbox Live. It is not part of the game disc.";
  options.target_directory = game_root.string();
  options.initial_status =
      "Download it automatically, or select a title update package you already have.";
  options.fetch_button_label = "Download (1.7 MB)";
  options.pick_button_label = "Select file...";
  options.fetch_connecting_status =
      "Connecting to the download server... (this can take a moment)";
  options.fetch_working_status = "Downloading Title Update 3...";
  options.install_working_status = "Installing the title update...";
  options.done_status = "Title Update 3 installed.";
  options.done_button_label = "Start Game";

  auto fetch = [game_root](std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                           std::string& error) {
    if (!DownloadAndStageTitleUpdate(game_root, copied_bytes, total_bytes, error)) {
      return false;
    }
    if (!IsTitleUpdateInstalled(game_root)) {
      error = "The title update could not be verified after installation.";
      return false;
    }
    return true;
  };
  auto install = [game_root](const std::filesystem::path& source,
                             std::atomic<uint64_t>& copied_bytes,
                             std::atomic<uint64_t>& total_bytes, std::string& error) {
    (void)copied_bytes;
    (void)total_bytes;
    if (!StageTitleUpdateFromFile(source, game_root, error)) {
      return false;
    }
    if (!IsTitleUpdateInstalled(game_root)) {
      error = "The title update could not be verified after installation.";
      return false;
    }
    return true;
  };

  new rex::ui::AcquireWizardDialog(
      drawer, std::move(options), std::move(fetch), []() { return PickTitleUpdateFile(); },
      std::move(install),
      [runtime_paths = std::move(runtime_paths), complete = std::move(complete)]() mutable {
        if (complete) {
          complete(std::move(runtime_paths));
        }
      });
}

bool RunTitleUpdateInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                         rex::ui::Window* window,
                                         rex::ui::ImGuiDrawer* drawer,
                                         rex::PathConfig runtime_paths,
                                         rex::PathConfig& installed_paths) {
  if (const char* automated_tu = std::getenv("SKATE3_INSTALL_TU");
      automated_tu != nullptr && *automated_tu != '\0') {
    std::string error;
    bool ok = false;
    REXLOG_INFO("Installing Skate 3 title update from SKATE3_INSTALL_TU={}", automated_tu);
    if (std::string_view(automated_tu) == "download") {
      std::atomic<uint64_t> copied_bytes{0};
      std::atomic<uint64_t> total_bytes{0};
      ok = DownloadAndStageTitleUpdate(runtime_paths.game_data_root, copied_bytes, total_bytes,
                                       error);
    } else {
      ok = StageTitleUpdateFromFile(automated_tu, runtime_paths.game_data_root, error);
    }
    if (!ok || !IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
      REXLOG_ERROR("Automated title update installation failed: {}", error);
      return false;
    }
    installed_paths = std::move(runtime_paths);
    REXLOG_INFO("Automated title update installation completed successfully");
    return true;
  }

  struct InstallResult {
    bool done = false;
    bool ok = false;
    rex::PathConfig paths;
  };
  auto result = std::make_shared<InstallResult>();

  ShowTitleUpdateInstallWizard(drawer, runtime_paths,
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

  REXLOG_INFO("Entering Skate 3 title update installer pump");
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
    REXLOG_INFO("Leaving title update installer pump without installation");
    return false;
  }

  installed_paths = std::move(result->paths);
  REXLOG_INFO("Leaving title update installer pump after successful installation");
  return true;
}

}  // namespace skate3
