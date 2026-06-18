#include "skate3_app_common.h"

#include "skate3_fov.h"
#include "skate3_icon_data.h"
#include "skate3_iso_installer.h"
#include "skate3_profile_manager.h"
#include "skate3_title_update_installer.h"
#include "skate3_dlc_installer.h"
#include "skate3_setup_summary.h"
#include "skate3_user_settings.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/ultrawide_debug.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <toml++/toml.h>
#include <cstdio>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/simple_settings_overlay.h>
#include <rex/ui/overlay/profile_creation_overlay.h>
#include <rex/ui/overlay/ultrawide_targets_overlay.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#if defined(__linux__) || defined(__APPLE__)
extern char** environ;
#endif

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

// Register multi-entry-function alternate entries that rex's analyzer can't
// lift via config because they overlap with auto-discovered parent functions.
extern "C" REX_FUNC(__restgprlr_19);

REXCVAR_DEFINE_STRING(skate3_dlc_root, "", "Skate 3",
                      "Directory containing Skate 3 DLC package files");
REXCVAR_DEFINE_BOOL(skate3_auto_install_dlc, true, "Skate 3",
                    "Install DLC package files found in configured DLC folders");
REXCVAR_DEFINE_BOOL(skate3_ultrawide, false, "Skate 3",
                    "Automatically derive an ultrawide guest video mode from the host display");
REXCVAR_DEFINE_DOUBLE(skate3_field_of_view, 60.0, "Skate 3",
                      "Gameplay camera field of view in degrees")
    .range(40.0, 120.0);
REXCVAR_DEFINE_INT32(skate3_ultrawide_base_height, 720, "Skate 3",
                     "Guest video mode height used when deriving ultrawide modes")
    .range(480, 2160)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_ultrawide_hor_plus, true, "Skate 3",
                    "Apply Hor+ clip-space correction for ultrawide video modes");
REXCVAR_DEFINE_DOUBLE(skate3_ultrawide_hor_plus_scale, 0.0, "Skate 3",
                      "Manual Hor+ X scale override (0 = derive from host aspect)")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_ultrawide_disable_ndc_correction, false, "Skate 3",
                    "Diagnostic: disable the GPU NDC Hor+ correction while preserving guest-side "
                    "ultrawide diagnostics");
REXCVAR_DEFINE_BOOL(skate3_ultrawide_trace_draws, false, "Skate 3",
                    "Diagnostic: collect live draw fingerprints for the F7 ultrawide overlay");
REXCVAR_DEFINE_BOOL(skate3_ultrawide_object_stage_trace_to_disk, false, "Skate 3",
                    "Diagnostic: write object render-stage transition summaries to disk");
REXCVAR_DEFINE_BOOL(skate3_ultrawide_object_stage_trace_continuous, false, "Skate 3",
                    "Diagnostic: continuously trace object render-stage summaries");
REXCVAR_DEFINE_INT32(skate3_ultrawide_object_stage_trace_frames_remaining, 0, "Skate 3",
                     "Diagnostic: trace this many object render-stage frames")
    .range(0, 1000000);
REXCVAR_DEFINE_INT32(skate3_ultrawide_object_stage_marker, 0, "Skate 3",
                     "Diagnostic: mark the next object-stage frame (1 visible, 2 invisible)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(skate3_ultrawide_object_stage_missing_limit, 256, "Skate 3",
                     "Diagnostic: maximum missing-main objects to log per object-stage frame")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(skate3_ultrawide_force_main_visibility_flags, false, "Skate 3",
                    "Force main render object visibility flags while Hor+ ultrawide is active");
REXCVAR_DEFINE_BOOL(skate3_ultrawide_widen_game_frustum, true, "Skate 3",
                    "Widen the game-side main-world cull frustum to match the ultrawide view");
REXCVAR_DEFINE_DOUBLE(skate3_ultrawide_target_aspect, 0.0, "Skate 3",
                      "Derived host display aspect for ultrawide presentation (0 = disabled)")
    .range(0.0, 8.0);

namespace {

#if defined(__linux__) || defined(__APPLE__)
std::vector<std::string> CurrentProcessArgumentsForRestart(
    const std::filesystem::path& executable_path) {
  std::vector<std::string> args;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  args.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i] ? argv[i] : "");
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  char ch = 0;
  while (cmdline.get(ch)) {
    if (ch == '\0') {
      args.push_back(std::move(arg));
      arg.clear();
    } else {
      arg.push_back(ch);
    }
  }
  if (!arg.empty()) {
    args.push_back(std::move(arg));
  }
#endif

  const std::string executable = rex::path_to_utf8(executable_path);
  if (args.empty()) {
    args.push_back(executable);
  } else {
    args[0] = executable;
  }
  return args;
}

void SetRestartArgument(std::vector<std::string>& args, std::string name, std::string value) {
  const std::string option = "--" + name;
  const std::string option_with_equals = option + "=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == option) {
      if (i + 1 < args.size()) {
        args[i + 1] = std::move(value);
      } else {
        args.push_back(std::move(value));
      }
      return;
    }
    if (args[i].starts_with(option_with_equals)) {
      args[i] = option_with_equals + value;
      return;
    }
  }

  args.push_back(std::move(option));
  args.push_back(std::move(value));
}
#endif

constexpr std::string_view kUserDirectoryName = "skate3";
constexpr std::string_view kSettingsFilename = "settings.toml";
constexpr std::string_view kDlcDirectoryName = "dlc";
constexpr double kSixteenNineAspect = 16.0 / 9.0;
constexpr double kUltrawideAspectEpsilon = 0.01;

struct DisplaySize {
  int32_t width = 0;
  int32_t height = 0;
};

#if defined(_WIN32)
BOOL CALLBACK CollectMonitorCallback(HMONITOR monitor_handle, HDC, LPRECT, LPARAM data) {
  auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
  monitors->push_back(monitor_handle);
  return TRUE;
}

HMONITOR GetConfiguredMonitorHandle() {
  const int32_t monitor_index = REXCVAR_GET(monitor);
  if (monitor_index > 0) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorCallback,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitor_index <= static_cast<int32_t>(monitors.size())) {
      return monitors[monitor_index - 1];
    }
  }

  POINT origin{0, 0};
  return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  HMONITOR monitor_handle = GetConfiguredMonitorHandle();
  if (!monitor_handle) {
    return std::nullopt;
  }

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!GetMonitorInfo(monitor_handle, &monitor_info)) {
    return std::nullopt;
  }

  const int32_t width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
  const int32_t height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  return DisplaySize{width, height};
}
#else
std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  return std::nullopt;
}
#endif

std::optional<DisplaySize> ResolveUltrawideTargetDisplaySize() {
  if (rex::cvar::HasNonDefaultValue("resolution")) {
    return std::nullopt;
  }

  if (REXCVAR_GET(fullscreen)) {
    return QueryFullscreenMonitorSize();
  }

  const int32_t configured_window_width = REXCVAR_GET(window_width);
  const int32_t configured_window_height = REXCVAR_GET(window_height);
  if (rex::cvar::HasNonDefaultValue("window_width") &&
      rex::cvar::HasNonDefaultValue("window_height") && configured_window_width > 0 &&
      configured_window_height > 0) {
    return DisplaySize{configured_window_width, configured_window_height};
  }

  return std::nullopt;
}

void ApplyUltrawideVideoDefaults() {
  if (!REXCVAR_GET(skate3_ultrawide) ||
      rex::cvar::HasNonDefaultValue("skate3_ultrawide_target_aspect")) {
    return;
  }

  const std::optional<DisplaySize> target_size = ResolveUltrawideTargetDisplaySize();
  if (!target_size || target_size->width <= 0 || target_size->height <= 0) {
    return;
  }

  const double target_aspect =
      static_cast<double>(target_size->width) / static_cast<double>(target_size->height);
  if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
    return;
  }

  rex::cvar::SetFlagByName("skate3_ultrawide_target_aspect", std::to_string(target_aspect));

  if (!rex::cvar::HasNonDefaultValue("present_letterbox")) {
    rex::cvar::SetFlagByName("present_letterbox", "true");
  }
}

void DisableActiveUltrawideDiagnostics() {
  constexpr std::string_view kFalseFlags[] = {
      "skate3_ultrawide_trace_draws",
      "skate3_ultrawide_object_stage_trace_to_disk",
      "skate3_ultrawide_object_stage_trace_continuous",
      "skate3_ultrawide_force_main_visibility_flags",
      "skate3_ultrawide_texture_trace_bind_keys",
      "skate3_ultrawide_texture_trace_scaled_resolve",
      "skate3_ultrawide_ignore_streamer_texture_invalidations",
      "skate3_ultrawide_screen_callback_tracking",
      "skate3_ultrawide_fake_occlusion_queries",
      "perf_draw_fingerprints",
      "perf_keep_heavyweight_draw_diagnostics",
      "trace_gpu_stream",
  };
  for (std::string_view flag : kFalseFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "false");
  }

  constexpr std::string_view kZeroFlags[] = {
      "skate3_ultrawide_texture_trace_remaining",
      "skate3_ultrawide_object_stage_trace_frames_remaining",
      "skate3_ultrawide_object_stage_marker",
      "vulkan_debug_log_frame_summaries_remaining",
      "vulkan_debug_log_resolve_decisions_remaining",
      "vulkan_debug_log_team_profile_background_candidates_remaining",
      "vulkan_debug_log_team_profile_background_bindings_remaining",
      "filesystem_debug_log_fe_asset_ops_remaining",
      "filesystem_debug_log_team_profile_background_remaining",
  };
  for (std::string_view flag : kZeroFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "0");
  }

  rex::cvar::SetFlagByName("skate3_ultrawide_fake_occlusion_sample_count", "1000");
}

std::filesystem::path DefaultDocumentsUserRoot() {
  return rex::filesystem::GetUserFolder() / std::string(kUserDirectoryName);
}

std::filesystem::path DefaultRoamingUserRoot() {
#if defined(_WIN32)
  char* appdata = nullptr;
  size_t appdata_size = 0;
  if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata && *appdata) {
    std::filesystem::path result = std::filesystem::path(appdata) / std::string(kUserDirectoryName);
    std::free(appdata);
    return result;
  }
  std::free(appdata);
#endif
  return DefaultDocumentsUserRoot();
}

std::filesystem::path ResolveSkate3UserRoot(const rex::PathConfig& paths) {
  const auto executable_root = rex::filesystem::GetExecutableFolder();
  if (std::filesystem::exists(executable_root / "portable.txt")) {
    return executable_root;
  }

  const auto old_default = DefaultDocumentsUserRoot();
  if (!paths.user_data_root.empty() && paths.user_data_root != old_default) {
    return paths.user_data_root;
  }

  return DefaultRoamingUserRoot();
}

void ConfigureSkate3UserPaths(rex::PathConfig& paths, std::filesystem::path& settings_path,
                              std::filesystem::path& profiles_path) {
  const auto old_user_root = DefaultDocumentsUserRoot();
  const auto old_cache_root = old_user_root / "cache";
  const auto original_cache_root = paths.cache_root;
  const auto resolved_user_root = ResolveSkate3UserRoot(paths);

  paths.user_data_root = resolved_user_root;
  if (original_cache_root.empty() || original_cache_root == old_cache_root) {
    paths.cache_root = resolved_user_root / "cache";
  }
  settings_path = resolved_user_root / std::string(kSettingsFilename);
  profiles_path = skate3::ProfilesFilePath(resolved_user_root);
}

bool ConfigContainsAnyKey(const toml::table& table, std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    if (table.contains(key)) {
      return true;
    }
  }
  return false;
}

bool ConfigFileContainsAnyKey(const std::filesystem::path& config_path,
                              std::initializer_list<std::string_view> keys) {
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return false;
  }
  try {
    auto config = toml::parse_file(config_path.string());
    return ConfigContainsAnyKey(config, keys);
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool DeveloperConfigHasResolutionScaleOverride(const std::filesystem::path& config_path) {
  return ConfigFileContainsAnyKey(config_path, {"resolution_scale", "draw_resolution_scale_x",
                                                "draw_resolution_scale_y"});
}

#if REX_PLATFORM_MAC
constexpr int kDefaultResolutionScale = 1;
#else
constexpr int kDefaultResolutionScale = 2;
#endif

void ApplyFirstRunVideoDefaults(const std::filesystem::path& settings_path,
                                const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path) ||
      DeveloperConfigHasResolutionScaleOverride(developer_config_path) ||
      rex::cvar::HasNonDefaultValue("resolution_scale") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_x") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")) {
    return;
  }

  const auto scale = std::to_string(kDefaultResolutionScale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
}

void LoadAndNormalizeSimpleSettings(const std::filesystem::path& settings_path,
                                    const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path)) {
    rex::cvar::LoadConfig(settings_path);
  } else {
    ApplyFirstRunVideoDefaults(settings_path, developer_config_path);
  }
  rex::ui::EnsureSimpleSettingsConfig(settings_path);
}

std::filesystem::path ResolveRuntimeGameDataRoot(const rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return paths.game_data_root;
  }

  const auto working_directory_game = std::filesystem::current_path() / "game";
  if (skate3::IsGameInstalled(working_directory_game)) {
    return working_directory_game;
  }

  return paths.config_path.parent_path() / "game";
}

const char* FirstExistingFontPath(std::initializer_list<const char*> paths) {
  for (const char* path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

std::string Hex8(uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return stream.str();
}

std::filesystem::path InstalledMarketplaceContentPath(const std::filesystem::path& content_root,
                                                      uint32_t title_id,
                                                      const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "00000002" /
         package_path.filename();
}

std::filesystem::path InstalledMarketplaceHeaderPath(const std::filesystem::path& content_root,
                                                     uint32_t title_id,
                                                     const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "Headers" / "00000002" /
         (package_path.filename().string() + ".header");
}

bool IsInstalledMarketplaceContent(const std::filesystem::path& content_root, uint32_t title_id,
                                   const std::filesystem::path& package_path) {
  return std::filesystem::is_directory(
             InstalledMarketplaceContentPath(content_root, title_id, package_path)) &&
         std::filesystem::is_regular_file(
             InstalledMarketplaceHeaderPath(content_root, title_id, package_path));
}

std::vector<std::filesystem::path> DiscoverDlcSourceDirectories(
    const std::filesystem::path& executable_root, const std::filesystem::path& game_data_root,
    const std::filesystem::path& user_data_root) {
  std::vector<std::filesystem::path> dirs;
  auto add_dir = [&](std::filesystem::path dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    if (ec) {
      return;
    }
    for (const auto& existing : dirs) {
      if (std::filesystem::equivalent(existing, dir, ec)) {
        return;
      }
      ec.clear();
    }
    dirs.push_back(std::move(dir));
  };

  const std::string configured_root = REXCVAR_GET(skate3_dlc_root);
  if (!configured_root.empty()) {
    add_dir(configured_root);
  }
  add_dir(executable_root / std::string(kDlcDirectoryName));
  add_dir(game_data_root / std::string(kDlcDirectoryName));
  return dirs;
}

}  // namespace

#if !SKATE3_HAS_TITLE_UPDATE
// Retail sub_82EBAE4C: alternate entry into sub_82EBAE34 (37 vtable refs in .data).
// Body: lwz r3, 0x164(r31); addi r1, r31, 0x140; b __restgprlr_19
static void Sub82EBAE4CImpl(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0x164);
  ctx.r1.s64 = ctx.r31.s64 + 0x140;
  __restgprlr_19(ctx, base);
}
#endif

Skate3BaseApp::~Skate3BaseApp() = default;

void Skate3BaseApp::OnConfigurePaths(rex::PathConfig& paths) {
  ConfigureSkate3UserPaths(paths, user_settings_path_, profiles_path_);
  config_path_ = paths.config_path;
  LoadAndNormalizeSimpleSettings(user_settings_path_, config_path_);
  Skate3InitializeFieldOfViewOverride();
  ApplyUltrawideVideoDefaults();
  if (!rex::graphics::ultrawide_debug::LoadTargets(paths.cache_root /
                                                   "skate3_ultrawide_targets.toml")) {
    rex::graphics::ultrawide_debug::LoadBuiltInSkate3Classifier();
  }
  DisableActiveUltrawideDiagnostics();
}

void Skate3BaseApp::OnConfigureFonts(ImFontAtlas* atlas) {
  if (!atlas) {
    return;
  }

  atlas->Clear();
  ImFontConfig font_config;
  font_config.OversampleH = 2;
  font_config.OversampleV = 2;
  font_config.PixelSnapH = false;

  const char* base_font_path = FirstExistingFontPath({
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\Helvetica.ttf",
      "C:\\Windows\\Fonts\\helvetica.ttf",
      "C:\\Windows\\Fonts\\HelveticaNeue.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/SFCompact.ttf",
      "/System/Library/Fonts/HelveticaNeue.ttc",
      "/System/Library/Fonts/LucidaGrande.ttc",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/local/share/fonts/NotoSans-Regular.ttf",
      "/usr/local/share/fonts/LiberationSans-Regular.ttf",
#endif
  });

  if (base_font_path) {
    atlas->AddFontFromFileTTF(base_font_path, 16.0f, &font_config,
                              atlas->GetGlyphRangesDefault());
  } else {
    atlas->AddFontDefault();
  }

#if defined(_WIN32)
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_font_config,
                              atlas->GetGlyphRangesJapanese());
  }
#endif
}

std::optional<rex::PathConfig> Skate3BaseApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  config_path_ = defaults.config_path;
  user_settings_path_ = defaults.user_data_root / std::string(kSettingsFilename);
  profiles_path_ = skate3::ProfilesFilePath(defaults.user_data_root);

  auto profiles = skate3::LoadProfiles(profiles_path_);
  const bool has_profiles_file = std::filesystem::exists(profiles_path_);

  // Shared setup flow: resolve paths → ISO → TU → DLC → summary → resume.
  // Called after profile is ready (either created or loaded).
  auto runSetupFlow = [this, defaults, resume = std::move(resume)]() mutable {
    auto runtime_paths = defaults;
    runtime_paths.game_data_root = ResolveRuntimeGameDataRoot(runtime_paths);
    runtime_paths.config_path.clear();

    skate3::SetupSummary summary;
    bool setup_performed = false;

    // Load profile info from disk for the summary.
    if (std::filesystem::exists(profiles_path_)) {
      auto profs = skate3::LoadProfiles(profiles_path_);
      if (auto* prof = skate3::FindSelectedProfile(profs)) {
        summary.profile_created = true;
        summary.profile_name = prof->gamertag;
      }
    }

    if (!skate3::IsGameInstalled(runtime_paths.game_data_root)) {
      REXLOG_INFO("Game files not found at {}; launching rexglue ISO installer",
                  runtime_paths.game_data_root.string());
      rex::PathConfig installed_paths;
      const bool installed = skate3::RunRexglueIsoInstallWizardBlocking(
          app_context(), window(), imgui_drawer(), runtime_paths, installed_paths);
      if (!installed) {
        app_context().QuitFromUIThread();
        return;
      }
      runtime_paths = std::move(installed_paths);
      summary.iso_installed = true;
      summary.iso_path = runtime_paths.game_data_root.string();
      setup_performed = true;
    }

#if SKATE3_HAS_TITLE_UPDATE
    if (!skate3::IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
      REXLOG_INFO("Skate 3 Title Update 4 not staged at {}; launching title update installer",
                  runtime_paths.game_data_root.string());
      rex::PathConfig tu_paths;
      const bool tu_installed = skate3::RunTitleUpdateInstallWizardBlocking(
          app_context(), window(), imgui_drawer(), runtime_paths, tu_paths);
      if (!tu_installed) {
        app_context().QuitFromUIThread();
        return;
      }
      runtime_paths = std::move(tu_paths);
      summary.tu_installed = true;
      setup_performed = true;
    } else {
      summary.tu_already_present = true;
    }
#endif

    // DLC installer (optional) — skip if DLCs are already present or user skipped.
    const auto existing_dlc = runtime_paths.game_data_root / std::string(kDlcDirectoryName);
    const auto skip_marker = existing_dlc / ".skip";
    const bool dlc_dir_has_content =
        std::filesystem::is_directory(existing_dlc) &&
        std::filesystem::directory_iterator(existing_dlc) != std::filesystem::directory_iterator{};
    if (!skate3::IsGameInstalled(runtime_paths.game_data_root) ||
        (!dlc_dir_has_content && !std::filesystem::exists(skip_marker))) {
      rex::PathConfig dlc_paths;
      const bool dlc_done = skate3::RunDlcInstallWizardBlocking(
          app_context(), window(), imgui_drawer(), runtime_paths, dlc_paths);
      if (dlc_done) {
        runtime_paths = std::move(dlc_paths);
      } else {
        summary.dlc_skipped = true;
      }
      setup_performed = true;
    } else {
      REXLOG_INFO("DLC packages already present at {}; skipping DLC installer",
                  existing_dlc.string());
    }

    // Scan installed DLC to populate summary stats (always, unless user explicitly skipped).
    if (!summary.dlc_skipped && std::filesystem::is_directory(existing_dlc)) {
      uint64_t total_bytes = 0;
      std::error_code iter_ec;
      for (const auto& entry : std::filesystem::directory_iterator(existing_dlc, iter_ec)) {
        if (iter_ec) break;
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext == ".skip") continue;
        auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(entry.path());
        if (!header) continue;
        auto name = header->metadata.display_name(rex::filesystem::XLanguage::kEnglish);
        std::string display_name;
        if (!name.empty()) {
          display_name.assign(name.begin(), name.end());
        } else {
          display_name = entry.path().filename().string();
        }
        summary.dlc_package_names.push_back(display_name);
        total_bytes += std::filesystem::file_size(entry.path(), iter_ec);
      }
      summary.dlc_total_bytes = total_bytes;
      summary.dlc_packages_installed = summary.dlc_package_names.size();
    }

    // If the user clicked Finish (which calls QuitFromUIThread), do not start the game.
    if (app_context().HasQuitFromUIThread()) {
      return;
    }

    // Show setup summary only if a wizard actually ran.
    if (setup_performed) {
      skate3::RunSetupSummaryBlocking(app_context(), window(), imgui_drawer(), summary);
    }
    resume(std::move(runtime_paths));
  };

  // First run: show profile creation wizard before anything else
  if (!has_profiles_file && profiles.profiles.empty()) {
    REXLOG_INFO("First run detected; launching profile creation wizard");
    auto profiles_dir = defaults.user_data_root / "profiles";
    // Wrap runSetupFlow in shared_ptr so it can be deferred out of the ImGui
    // OnDraw callback (calling blocking dialogs from inside OnDraw deadlocks).
    auto flow_shared = std::make_shared<std::function<void()>>(std::move(runSetupFlow));
    auto wizard = new rex::ui::ProfileCreationDialog(
        imgui_drawer(), profiles_dir,
        [this, flow_shared, profiles_dir](
            rex::ui::ProfileCreationResult result) mutable {
          // Save the created profile
          skate3::ProfileManager mgr;
          skate3::XboxLiveProfile profile;
          profile.xuid = result.xuid;
          profile.gamertag = result.gamertag;
          profile.signed_in = result.signed_in;
          profile.live_signed_in = result.live_signed_in;
          profile.region = result.region;
          mgr.Save(profile, profiles_dir);

          // Apply cvars
          rex::cvar::SetFlagByName("selected_user_profile", profile.gamertag);
          rex::cvar::SetFlagByName("user_profile_name", profile.gamertag);
          rex::cvar::SetFlagByName("user_profile_xuid", profile.xuid);
          rex::cvar::SetFlagByName("user_profile_signed_in", "true");
          rex::cvar::SetFlagByName("user_live_signed_in",
                                   profile.live_signed_in ? "true" : "false");

          // Defer to next frame — must not create dialogs inside OnDraw.
          app_context().CallInUIThreadDeferred([flow_shared]() { (*flow_shared)(); });
        },
        [this, flow_shared, defaults]() mutable {
          // User skipped - create a basic local profile
          auto profiles = skate3::LoadProfiles(skate3::ProfilesFilePath(defaults.user_data_root));
          skate3::EnsureUsableProfileStore(profiles, "Player");
          if (auto* profile = skate3::FindSelectedProfile(profiles)) {
            skate3::ApplyProfileCvars(*profile);
          }
          app_context().CallInUIThreadDeferred([flow_shared]() { (*flow_shared)(); });
        });
    wizard->Show();
    return std::nullopt;
  }

  skate3::EnsureUsableProfileStore(profiles, "Player");
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
  }

  const bool has_config_file = std::filesystem::exists(defaults.config_path);
  const bool has_game_path = std::filesystem::is_directory(defaults.game_data_root);
  if (!has_profiles_file && has_config_file && has_game_path) {
    skate3::SaveProfiles(profiles_path_, profiles);
  }

  runSetupFlow();
  return std::nullopt;
}

void Skate3BaseApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  rex::ui::RegisterBind("bind_skate3_menu", "Escape", "Skate 3 settings", [this] {
    ToggleSimpleSettings();
  });
  rex::ui::RegisterBind("bind_skate3_menu_alt", "F1", "Skate 3 settings alternate", [this] {
    ToggleSimpleSettings();
  });
  rex::ui::RegisterBind("bind_skate3_ultrawide_targets", "F7",
                        "Skate 3 ultrawide targets", [this] {
                          ToggleUltrawideTargets();
                        });
  rex::ui::RegisterBind("bind_skate3_save_draw_fingerprints", "F8",
                        "Save draw fingerprint log", [this] {
                          SaveDrawFingerprintLog();
                        });
  rex::ui::RegisterBind("bind_skate3_log_debug_marker", "F9",
                        "Write debug marker to log", [this] {
                          LogDebugMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_log_user_marker", "F10",
                        "Write marker to log", [this] {
                          LogUserMarker();
                        });
}

// Memory dump function
static void dump_game_memory() {
  auto mem = REX_KERNEL_MEMORY();
  if (!mem) return;
  uint8_t* base = (uint8_t*)mem->TranslateVirtual(0x80000000);
  if (!base) return;
  
  const uint32_t SCAN_SIZE = 0x04000000; // 64MB
  const char* path = "/home/rexx/Escritorio/Skate3_XEX/dumps/memdump_80000000_84000000.bin";
  FILE* f = fopen(path, "wb");
  if (f) {
    fwrite(base, 1, SCAN_SIZE, f);
    fclose(f);
    REXKRNL_INFO("[NET] Memory dump: {}MB -> {}", SCAN_SIZE / (1024*1024), path);
  }
  
  // Log string locations
  for (uint32_t off = 0; off < SCAN_SIZE - 12; off++) {
    if (memcmp(base + off, "nosecure", 8) == 0)
      REXKRNL_INFO("[NET] 'nosecure' at 0x{:08X}", 0x80000000 + off);
    if (memcmp(base + off, "-syslink", 8) == 0)
      REXKRNL_INFO("[NET] '-syslink' at 0x{:08X}", 0x80000000 + off);
  }
}

// Function override system
struct FuncOverride {
  uint32_t address;
  std::string patch_hex;
  std::string hook_name;
};

// XLSP bypass hook
static void xlsp_bypass_hook(PPCContext& ctx, uint8_t* /*base*/) {
  ctx.r3.s64 = 1;
  REXKRNL_INFO("[NET] XLSP bypass: forcing r3=1");
}

static void load_and_apply_overrides() {
  auto* dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  if (!dispatcher) return;
  
  // Debug controls
  bool debug_net = true;
  bool debug_dump = true;
  
  std::string config_path = "/home/rexx/.local/share/skate3/settings.toml";
  try {
    auto tbl = toml::parse_file(config_path);
    
    if (auto debug = tbl["debug"].as_table()) {
      debug_net = debug->get("net")->value_or(true);
      debug_dump = debug->get("dump")->value_or(true);
      REXKRNL_INFO("[NET] Debug: net={}, dump={}", debug_net, debug_dump);
    }
    
    if (debug_dump) dump_game_memory();
    
    if (auto overrides = tbl["function_overrides"].as_table()) {
      for (auto& [key, val] : *overrides) {
        std::string key_str(key);
        uint32_t addr = std::stoul(key_str, nullptr, 16);
        std::string val_str;
        if (val.is_string()) val_str = val.value_or<std::string>("");
        
        if (val_str.size() > 2 && val_str.substr(0, 2) == "0x") {
          // Raw memory patch
          uint8_t* target = (uint8_t*)REX_KERNEL_MEMORY()->TranslateVirtual(addr);
          if (target) {
            std::string hex = val_str.substr(2);
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
              uint8_t byte = (uint8_t)std::stoul(hex.substr(i, 2), nullptr, 16);
              target[i / 2] = byte;
            }
            REXKRNL_INFO("[NET] Override: patched 0x{:08X} with {}", addr, hex);
          }
        } else if (val_str == "xlsp_bypass") {
          dispatcher->SetFunction(addr, &xlsp_bypass_hook);
          REXKRNL_INFO("[NET] Override: SetFunction 0x{:08X} -> xlsp_bypass", addr);
        }
      }
    }
  } catch (const std::exception& e) {
    REXKRNL_INFO("[NET] No overrides: {}", e.what());
  }
}

void Skate3BaseApp::OnPostSetup() {
  // Load function overrides and dump memory right after TU is applied
  load_and_apply_overrides();

  // Set embedded window icon from compiled PNG data
  if (window()) {
    window()->SetIcon(skate3_icon_png, skate3_icon_png_len);
  }

  ApplySelectedProfileToRuntime();
  ApplyGameplayCursorMode();

  if (auto* input_system = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    input_system->SetActiveCallback([this]() {
      const bool settings_visible = simple_settings_dialog_ && simple_settings_dialog_->visible();
      const bool xam_ui_active = rex::kernel::xam::xeXamIsUIActive();
      return !settings_visible && !xam_ui_active;
    });
    input_system->SetMenuChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() { ToggleSimpleSettings(); });
    });
  }

  if (std::getenv("SKATE3_DISABLE_BIG_ALIASES") == nullptr) {
    InstallBigDeviceAliases();
  }
  InstallDlcPackages();
  InstallRecipeOverlay();

  // Register retail multi-entry-function alternate entries.
#if !SKATE3_HAS_TITLE_UPDATE
  runtime()->function_dispatcher()->SetFunction(0x82EBAE4C, &Sub82EBAE4CImpl);
#endif

  auto* dispatcher = runtime()->function_dispatcher();
  if (dispatcher->InitializeFunctionTable(eawebkit_PPCImageConfig.code_base,
                                          eawebkit_PPCImageConfig.code_size,
                                          eawebkit_PPCImageConfig.image_base,
                                          eawebkit_PPCImageConfig.image_size)) {
    for (int i = 0; eawebkit_PPCImageConfig.func_mappings[i].guest != 0; ++i) {
      auto* host = eawebkit_PPCImageConfig.func_mappings[i].host;
      if (host) {
        dispatcher->SetFunction(
            static_cast<uint32_t>(eawebkit_PPCImageConfig.func_mappings[i].guest),
            host);
      }
    }
  }
}

void Skate3BaseApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_skate3_menu");
  rex::ui::UnregisterBind("bind_skate3_menu_alt");
  rex::ui::UnregisterBind("bind_skate3_ultrawide_targets");
  rex::ui::UnregisterBind("bind_skate3_save_draw_fingerprints");
  rex::ui::UnregisterBind("bind_skate3_log_debug_marker");
  rex::ui::UnregisterBind("bind_skate3_log_user_marker");
  ApplyGameplayCursorMode();
  simple_settings_dialog_.reset();
  ultrawide_targets_dialog_.reset();
}

void Skate3BaseApp::ToggleSimpleSettings() {
  if (simple_settings_dialog_) {
    if (simple_settings_dialog_->visible()) {
      simple_settings_dialog_->Hide();
    } else {
      ApplySettingsCursorMode();
      simple_settings_dialog_->Show();
    }
    return;
  }

  auto load_profiles = [this]() {
    rex::ui::SimpleProfileState state;
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    state.selected_index = 0;
    for (int i = 0; i < static_cast<int>(store.profiles.size()); ++i) {
      const auto& profile = store.profiles[i];
      state.profiles.push_back({profile.id, profile.gamertag, profile.signed_in});
      if (profile.id == store.selected_profile) {
        state.selected_index = i;
      }
    }
    return state;
  };
  auto save_profile = [this](int selected_index, std::string gamertag, bool signed_in) {
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    if (store.profiles.empty()) {
      return;
    }
    selected_index = std::clamp(selected_index, 0, static_cast<int>(store.profiles.size()) - 1);
    auto& profile = store.profiles[selected_index];
    if (!gamertag.empty()) {
      profile.gamertag = std::move(gamertag);
    }
    profile.signed_in = signed_in;
    if (!profile.signed_in) {
      profile.live_signed_in = false;
    }
    store.selected_profile = profile.id;
    skate3::SaveProfiles(profiles_path_, store);
    skate3::ApplyProfileCvars(profile);
    ApplySelectedProfileToRuntime();
  };
  auto close_settings = [this]() { ApplyGameplayCursorMode(); };
  auto close_game = [this]() {
#if REX_PLATFORM_MAC || REX_PLATFORM_LINUX
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      REXLOG_WARN("Close Game watchdog exiting process after shutdown timeout");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#endif
    app_context().CallInUIThreadDeferred([this]() {
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      if (window()) {
        window()->RequestClose();
      } else {
        app_context().QuitFromUIThread();
      }
#endif
    });
  };
  auto restart_game = [this]() { RestartGame(); };
  simple_settings_dialog_ =
      std::make_unique<rex::ui::SimpleSettingsDialog>(
          imgui_drawer(), user_settings_path_, std::move(load_profiles), std::move(save_profile),
          std::move(close_settings), std::move(close_game), std::move(restart_game));
  ApplySettingsCursorMode();
  simple_settings_dialog_->Show();
}

void Skate3BaseApp::ToggleUltrawideTargets() {
  if (ultrawide_targets_dialog_) {
    if (ultrawide_targets_dialog_->visible()) {
      ultrawide_targets_dialog_->Hide();
    } else {
      ApplySettingsCursorMode();
      ultrawide_targets_dialog_->Show();
    }
    return;
  }

  const auto export_path = cache_root() / "skate3_ultrawide_targets.toml";
  ultrawide_targets_dialog_ = std::make_unique<rex::ui::UltrawideTargetsDialog>(
      imgui_drawer(), export_path, [this]() { ApplyGameplayCursorMode(); });
  ApplySettingsCursorMode();
  ultrawide_targets_dialog_->Show();
}

void Skate3BaseApp::ApplySettingsCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void Skate3BaseApp::ApplyGameplayCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void Skate3BaseApp::RestartGame() {
  app_context().CallInUIThreadDeferred([this]() {
#if defined(_WIN32)
    wchar_t executable_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup_info, &process_info)) {
      REXLOG_WARN("Restart requested, but launching a new process failed");
      return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#elif defined(__linux__) || defined(__APPLE__)
    const auto executable_path = rex::filesystem::GetExecutablePath();
    if (executable_path.empty()) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    auto args = CurrentProcessArgumentsForRestart(executable_path);
    std::filesystem::path restart_game_data_root;
    if (runtime()) {
      restart_game_data_root = runtime()->game_data_root();
    }
    if (restart_game_data_root.empty()) {
      restart_game_data_root = game_data_root();
    }
    if (!restart_game_data_root.empty()) {
      SetRestartArgument(args, "game_data_root", rex::path_to_utf8(restart_game_data_root));
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const std::string executable = rex::path_to_utf8(executable_path);
    pid_t child_pid = 0;
    const int spawn_result =
        posix_spawn(&child_pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
      REXLOG_WARN("Restart requested, but launching a new process failed: {}",
                  std::strerror(spawn_result));
      return;
    }
#else
    REXLOG_WARN("Restart requested, but automatic restart is not implemented on this platform");
    return;
#endif

  #if REX_PLATFORM_MAC
    app_context().QuitFromUIThread();
  #else
    if (window()) {
      window()->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  #endif
  });
}

void Skate3BaseApp::SaveDrawFingerprintLog() {
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  REXLOG_WARN("Perf capture is unavailable because perf counters are disabled in this build");
  return;
#else
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream counters_filename;
  counters_filename << "perf_capture_counters_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
                    << ".csv";
  std::ostringstream filename;
  filename << "perf_capture_draw_fingerprints_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
           << ".csv";

  std::error_code ec;
  std::filesystem::create_directories(cache_root(), ec);
  const auto counters_path = cache_root() / counters_filename.str();
  const auto path = cache_root() / filename.str();
  if (rex::perf::StartCapture(counters_path, path)) {
    REXLOG_INFO("Started perf capture; counter log will be saved to {}",
                counters_path.string());
    REXLOG_INFO("Started perf capture; draw fingerprint log will be saved to {}",
                path.string());
  } else {
    REXLOG_WARN("Perf capture is already running");
  }
#endif
}

void Skate3BaseApp::LogUserMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("USER LOG MARKER #{}: F10 pressed", marker);
}

void Skate3BaseApp::LogDebugMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  DisableActiveUltrawideDiagnostics();
  REXLOG_WARN("USER DEBUG MARKER #{}: F9 pressed; active diagnostics disabled", marker);
}

void Skate3BaseApp::ApplySelectedProfileToRuntime() {
  if (!runtime() || !runtime()->kernel_state() || !runtime()->kernel_state()->user_profile()) {
    return;
  }
  auto profiles = skate3::LoadProfiles(profiles_path_);
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    runtime()->kernel_state()->user_profile()->SetIdentity(profile->xuid, profile->gamertag);
  }
}

bool Skate3BaseApp::IsRecipeNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::set<std::string> Skate3BaseApp::DiscoverRecipeAliases(
    const std::filesystem::path& content_root) {
  static constexpr size_t kScanBytes = 8 * 1024 * 1024;
  static constexpr std::string_view kPrefix = "data/content/recipe/";

  std::set<std::string> aliases;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }

    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      continue;
    }

    std::string buffer;
    buffer.resize(kScanBytes);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(stream.gcount()));

    size_t pos = 0;
    while ((pos = buffer.find(kPrefix, pos)) != std::string::npos) {
      size_t name_start = pos + kPrefix.size();
      size_t name_end = name_start;
      while (name_end < buffer.size() && IsRecipeNameChar(buffer[name_end])) {
        ++name_end;
      }
      if (name_end > name_start) {
        aliases.insert(buffer.substr(name_start, name_end - name_start));
      }
      pos = name_end;
    }
  }

  return aliases;
}

bool Skate3BaseApp::CreateOverlayDirectory(
    const std::filesystem::path& overlay_root, std::string_view guest_path) {
  std::filesystem::path path = overlay_root;
  size_t segment_start = 0;
  while (segment_start < guest_path.size()) {
    const size_t slash = guest_path.find('/', segment_start);
    const size_t segment_end = slash == std::string_view::npos ? guest_path.size() : slash;
    if (segment_end > segment_start) {
      path /= std::string(guest_path.substr(segment_start, segment_end - segment_start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

void Skate3BaseApp::InstallRecipeOverlay() {
  if (recipe_overlay_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  const auto content_root = runtime()->game_data_root() / "data" / "content";
  if (!std::filesystem::exists(content_root)) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; content root not found: {}",
                content_root.string());
    return;
  }

  const auto aliases = DiscoverRecipeAliases(content_root);
  if (aliases.empty()) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; no recipe aliases found in {}",
                content_root.string());
    return;
  }

  const auto overlay_root = cache_root() / "vfs_big_directory_aliases";
  std::error_code ec;
  std::filesystem::create_directories(overlay_root, ec);
  if (ec) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create {}: {}",
                overlay_root.string(), ec.message());
    return;
  }

  size_t created = 0;
  for (const auto& alias : aliases) {
    const std::string guest_path = std::string("data/content/recipe/") + alias;
    if (CreateOverlayDirectory(overlay_root, guest_path)) {
      ++created;
    }
  }

  static constexpr std::string_view kBigDirectoryAliases[] = {
      "data/scene",
      "data/scene/anim",
      "data/scene/trickguide",
      "data/livingworld/PluginDescriptor",
      "data/state/livingworldentities/pedestrian/plugin",
  };
  for (std::string_view alias : kBigDirectoryAliases) {
    if (CreateOverlayDirectory(overlay_root, alias)) {
      ++created;
    }
  }

  if (!created) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create alias directories in {}",
                overlay_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      "skate3bigdirs:", overlay_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to initialize host device for {}",
                overlay_root.string());
    return;
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\content\\recipe",
      "skate3bigdirs:\\data\\content\\recipe");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\scene",
      "skate3bigdirs:\\data\\scene");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\livingworld\\PluginDescriptor",
      "skate3bigdirs:\\data\\livingworld\\PluginDescriptor");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\state\\livingworldentities\\pedestrian\\plugin",
      "skate3bigdirs:\\data\\state\\livingworldentities\\pedestrian\\plugin");
  recipe_overlay_installed_ = true;
  REXLOG_INFO("Installed Skate 3 BIG-directory VFS overlay with {} aliases from {}",
              created, content_root.string());
}

void Skate3BaseApp::InstallBigDeviceAliases() {
  if (big_device_aliases_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  // Skate 3 uses these file-server style schemes for world content probes.
  // Route them to the normal title mount so existing disc files resolve.
  runtime()->file_system()->RegisterSymbolicLink(
      "big:", "\\Device\\Harddisk0\\Partition1");
  runtime()->file_system()->RegisterSymbolicLink(
      "dlcbig:", "\\Device\\Harddisk0\\Partition1");
  big_device_aliases_installed_ = true;
}

void Skate3BaseApp::InstallDlcPackages() {
  if (!REXCVAR_GET(skate3_auto_install_dlc) || !runtime() || !runtime()->kernel_state() ||
      !runtime()->kernel_state()->content_manager()) {
    return;
  }

  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id == 0) {
    REXLOG_WARN("Skipping Skate 3 DLC install; title ID is not available");
    return;
  }

  const auto user_dlc_root = runtime()->user_data_root() / std::string(kDlcDirectoryName);
  std::error_code create_ec;
  std::filesystem::create_directories(user_dlc_root, create_ec);
  if (create_ec) {
    REXLOG_WARN("Could not create Skate 3 DLC drop folder {}: {}", user_dlc_root.string(),
                create_ec.message());
  }

  const auto source_dirs =
      DiscoverDlcSourceDirectories(rex::filesystem::GetExecutableFolder(),
                                   runtime()->game_data_root(), runtime()->user_data_root());
  std::unordered_set<std::string> seen_packages;
  size_t installed_count = 0;
  size_t skipped_count = 0;

  for (const auto& source_dir : source_dirs) {
    if (!std::filesystem::is_directory(source_dir)) {
      continue;
    }

    std::error_code iter_ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_dir, iter_ec)) {
      if (iter_ec) {
        REXLOG_WARN("Could not scan Skate 3 DLC folder {}: {}", source_dir.string(),
                    iter_ec.message());
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto package_path = entry.path();
      std::error_code canonical_ec;
      auto package_key = std::filesystem::weakly_canonical(package_path, canonical_ec).string();
      if (canonical_ec) {
        package_key = std::filesystem::absolute(package_path).string();
      }
      if (!seen_packages.insert(package_key).second) {
        continue;
      }

      const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
      if (!header) {
        REXLOG_WARN("Skipping DLC candidate with invalid STFS header: {}", package_path.string());
        ++skipped_count;
        continue;
      }

      const auto content_type =
          static_cast<rex::system::XContentType>(header->metadata.content_type);
      if (content_type != rex::system::XContentType::kMarketplaceContent) {
        REXLOG_WARN("Skipping non-DLC content package {} with type {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(content_type));
        ++skipped_count;
        continue;
      }

      const uint32_t package_title_id = header->metadata.execution_info.title_id;
      if (package_title_id != 0 && package_title_id != title_id) {
        REXLOG_WARN("Skipping DLC package {} for title {:08X}; running title is {:08X}",
                    package_path.filename().string(), package_title_id, title_id);
        ++skipped_count;
        continue;
      }

      if (IsInstalledMarketplaceContent(runtime()->user_data_root(), title_id, package_path)) {
        REXLOG_INFO("DLC package already installed: {}", package_path.filename().string());
        ++skipped_count;
        continue;
      }

      REXLOG_INFO("Installing Skate 3 DLC package: {}", package_path.string());
      const auto result = runtime()->kernel_state()->content_manager()->InstallContent(package_path);
      if (result == 0) {
        ++installed_count;
        REXLOG_INFO("Installed Skate 3 DLC package: {}", package_path.filename().string());
      } else {
        ++skipped_count;
        REXLOG_WARN("Failed to install Skate 3 DLC package {}: {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(result));
      }
    }
  }

  if (installed_count || skipped_count) {
    REXLOG_INFO("Skate 3 DLC scan complete: {} installed, {} skipped", installed_count,
                skipped_count);
  } else {
    REXLOG_INFO("No Skate 3 DLC packages found. Drop legally obtained DLC package files in {}",
                user_dlc_root.string());
  }
}
