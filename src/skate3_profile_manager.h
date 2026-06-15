#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skate3 {

struct XboxLiveProfile {
  std::string xuid;           // Xbox 360 format: E000 + 13 hex chars
  std::string gamertag;
  bool signed_in = true;
  bool live_signed_in = true; // Xbox Live enabled
  uint32_t zone = 1;          // Xbox Live zone
  uint32_t region = 1;        // NA=1, EU=2, JP=3
  std::string motto;
  std::string gamer_picture;
};

class ProfileManager {
 public:
  // Generate Xbox 360 format XUID: E000 + 13 hex chars
  static std::string GenerateXuid(const std::string& gamertag);

  // Create a default Xbox Live profile
  static XboxLiveProfile CreateDefault(const std::string& gamertag);

  // Check if any profiles exist in the profiles directory
  static bool HasProfiles(const std::filesystem::path& profiles_dir);

  // Save a profile to the profiles directory
  bool Save(const XboxLiveProfile& profile, const std::filesystem::path& dir);

  // Load a profile from a TOML file
  XboxLiveProfile Load(const std::filesystem::path& file_path);

  // List all profiles in the profiles directory
  std::vector<XboxLiveProfile> ListAll(const std::filesystem::path& dir);
};

}  // namespace skate3
