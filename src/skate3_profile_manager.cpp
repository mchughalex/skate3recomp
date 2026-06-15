#include "skate3_profile_manager.h"
#include "skate3_user_settings.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <time.h>

#include <toml++/toml.hpp>

namespace skate3 {
namespace {

uint64_t FnvHash(std::string_view data) {
  uint64_t hash = 1469598103934665603ull;
  for (char c : data) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string ProfileTomlString(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

uint64_t ParseXuidString(std::string_view value) {
  std::string text(value);
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.erase(0, 2);
  }
  uint64_t parsed = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
  if (ec == std::errc() && ptr == text.data() + text.size()) {
    return parsed;
  }
  return 0;
}

}  // namespace

std::string ProfileManager::GenerateXuid(const std::string& gamertag) {
  // Xbox 360 XUID format: E000 + 12 hex chars
  // The E000 prefix indicates a local profile
  uint64_t hash = FnvHash(gamertag + std::to_string(time(nullptr)));
  std::ostringstream ss;
  ss << "E000" << std::hex << std::setw(12) << std::setfill('0') << (hash & 0xFFFFFFFFFFF);
  return ss.str();
}

XboxLiveProfile ProfileManager::CreateDefault(const std::string& gamertag) {
  XboxLiveProfile profile;
  profile.gamertag = gamertag.empty() ? "Player" : gamertag;
  profile.xuid = GenerateXuid(profile.gamertag);
  profile.signed_in = true;
  profile.live_signed_in = true;
  profile.zone = 1;
  profile.region = 1;
  profile.motto = "";
  profile.gamer_picture = "gamercard_picture_key";
  return profile;
}

bool ProfileManager::HasProfiles(const std::filesystem::path& profiles_dir) {
  auto profiles_file = profiles_dir / "profiles.toml";
  return std::filesystem::exists(profiles_file);
}

bool ProfileManager::Save(const XboxLiveProfile& profile,
                          const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return false;
  }

  auto profiles_file = dir / "profiles.toml";

  // Load existing profiles or create new
  LocalProfileStore store;
  if (std::filesystem::exists(profiles_file)) {
    try {
      auto table = toml::parse_file(profiles_file.string());
      store.selected_profile = table["selected_profile"].value_or(std::string{});
      if (auto profiles = table["profiles"].as_array()) {
        profiles->for_each([&](toml::table& profile_table) {
          LocalProfile p;
          p.id = profile_table["id"].value_or(std::string{});
          p.gamertag = profile_table["gamertag"].value_or(std::string{});
          p.signed_in = profile_table["signed_in"].value_or(true);
          p.live_signed_in = profile_table["live_signed_in"].value_or(false);
          if (auto xuid = profile_table["xuid"].value<std::string>()) {
            p.xuid = ParseXuidString(*xuid);
          }
          if (!p.id.empty() && !p.gamertag.empty() && p.xuid != 0) {
            store.profiles.push_back(std::move(p));
          }
        });
      }
    } catch (const toml::parse_error&) {
      store = {};
    }
  }

  // Add new profile
  LocalProfile new_profile;
  new_profile.id = profile.gamertag;
  new_profile.gamertag = profile.gamertag;
  new_profile.xuid = ParseXuidString(profile.xuid);
  new_profile.signed_in = profile.signed_in;
  new_profile.live_signed_in = profile.live_signed_in;
  store.profiles.push_back(std::move(new_profile));
  store.selected_profile = profile.gamertag;

  // Save
  std::ofstream file(profiles_file, std::ios::trunc);
  if (!file) {
    return false;
  }

  file << "selected_profile = " << ProfileTomlString(store.selected_profile) << "\n\n";
  for (const auto& p : store.profiles) {
    file << "[[profiles]]\n";
    file << "id = " << ProfileTomlString(p.id) << "\n";
    file << "gamertag = " << ProfileTomlString(p.gamertag) << "\n";
    file << "xuid = " << ProfileTomlString(FormatXuid(p.xuid)) << "\n";
    file << "signed_in = " << (p.signed_in ? "true" : "false") << "\n";
    file << "live_signed_in = " << (p.live_signed_in ? "true" : "false") << "\n\n";
  }

  return true;
}

XboxLiveProfile ProfileManager::Load(const std::filesystem::path& file_path) {
  XboxLiveProfile profile;
  if (!std::filesystem::exists(file_path)) {
    return profile;
  }

  try {
    auto table = toml::parse_file(file_path.string());
    profile.gamertag = table["gamertag"].value_or(std::string{"Player"});
    profile.signed_in = table["signed_in"].value_or(true);
    profile.live_signed_in = table["live_signed_in"].value_or(true);
    profile.zone = table["zone"].value_or<uint32_t>(1);
    profile.region = table["region"].value_or<uint32_t>(1);
    profile.motto = table["motto"].value_or(std::string{});
    profile.gamer_picture = table["gamer_picture"].value_or(std::string{"gamercard_picture_key"});
    if (auto xuid = table["xuid"].value<std::string>()) {
      profile.xuid = *xuid;
    }
  } catch (const toml::parse_error&) {
    profile = CreateDefault("Player");
  }

  return profile;
}

std::vector<XboxLiveProfile> ProfileManager::ListAll(
    const std::filesystem::path& dir) {
  std::vector<XboxLiveProfile> profiles;
  if (!std::filesystem::exists(dir)) {
    return profiles;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".toml") {
      auto profile = Load(entry.path());
      if (!profile.xuid.empty()) {
        profiles.push_back(std::move(profile));
      }
    }
  }

  return profiles;
}

}  // namespace skate3
