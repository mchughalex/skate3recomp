#pragma once

#include <filesystem>

namespace skate3 {

// Application icon sourced from the USER'S OWN game files at runtime; the
// shipped executable and the repository deliberately contain no EA artwork
// (same clean-distribution rule as every other game asset).
//
// Windows only (no-op elsewhere). Uses the 64x64 title tile PNG embedded
// in game/nxeart for the window/taskbar/Alt-Tab icon through WM_SETICON.
// Does not modify the executable, write icon resources, or start a helper
// process. Explorer/desktop shortcuts may use a separate external .ico file.
//
// Runs on a background thread; failures only log (the icon stays default).
void ApplyGameIconFromGameData(const std::filesystem::path& game_data_root,
                               void* native_window_handle);

}  // namespace skate3
