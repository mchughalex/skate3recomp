#include "skate3_demo_path.h"

#include "skate3_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_demo_path, false, "Skate 3",
                    "Probe and automate the boot path to gameplay");
REXCVAR_DEFINE_BOOL(skate3_demo_path_probe, false, "Skate 3",
                    "Log Skate 3 boot/frontend states used by the demo path");
REXCVAR_DEFINE_BOOL(skate3_demo_path_signed_in, false, "Skate 3",
                    "Demo path: keep the real signed-in profile (and its save) instead of "
                    "forcing a signed-out boot");
REXCVAR_DEFINE_STRING(skate3_demo_path_gameplay_inputs, "", "Skate 3",
                      "Demo path: comma-separated pad inputs injected once after gameplay "
                      "settles (tokens: a b x y start back lb rb lt rt up down left right l3 "
                      "r3; a ':ms' suffix overrides the delay AFTER that input, e.g. rt:1500). "
                      "Map switch to PCU Library: "
                      "start,a,rt:1500,rt:1500,down,down,a,down,down,down,down,down,a,a");
REXCVAR_DEFINE_INT32(skate3_demo_path_input_settle_ms, 2500, "Skate 3",
                     "Demo path: wait this long after gameplay is reached before injecting "
                     "skate3_demo_path_gameplay_inputs")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(skate3_demo_path_input_delay_ms, 600, "Skate 3",
                     "Demo path: delay between injected gameplay inputs")
    .range(50, 10000);
REXCVAR_DEFINE_BOOL(skate3_intro_movie_skip, true, "Skate 3",
                    "Skip the frontend intro movie when A or Start is pressed "
                    "(the default keyboard bindings make that Space and Enter)");

namespace skate3::demo_path {
namespace {

constexpr uint32_t kFrontEndStatePressStart = 24;
constexpr uint32_t kFrontEndStateLanguageSelect = 47;

std::atomic<uint32_t> g_last_requested_state{0};
std::atomic<bool> g_seen_language_update{false};
std::atomic<uint32_t> g_last_language_select_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_last_press_start_event{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_automation_stage{0};
std::atomic<bool> g_skip_intro_movie{false};
std::atomic<bool> g_logged_intro_movie_skip{false};
std::atomic<bool> g_f10_poll_was_down{false};

bool ProbeEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path") ||
         rex::cvar::Query<bool>("skate3_demo_path_probe");
}

bool AutomationEnabled() {
  return rex::cvar::Query<bool>("skate3_demo_path");
}

const char* KnownFrontEndStateName(uint32_t state_id) {
  switch (state_id) {
    case kFrontEndStatePressStart:
      return "press-start";
    case kFrontEndStateLanguageSelect:
      return "language-select";
    default:
      return "unknown";
  }
}

void LogBootFlowEventChange(std::atomic<uint32_t>& last_event, const char* name, uint32_t event,
                            uint32_t state_this) {
  if (!ProbeEnabled()) {
    return;
  }

  uint32_t previous = last_event.load(std::memory_order_relaxed);
  while (previous != event) {
    if (last_event.compare_exchange_weak(previous, event, std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: {} event={} this=0x{:08X}", name, event, state_this);
      return;
    }
  }
}

void QueueLanguageAcceptIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t expected = 0;
  if (!g_automation_stage.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
    return;
  }

  rex::kernel::xam::QueueSyntheticInput(rex::input::X_INPUT_GAMEPAD_A, 8);
  REXLOG_INFO("Skate 3 demo path: queued language-select A pulse");
}

void EnableTitleStartAutoTapIfNeeded(const char* reason) {
  if (!AutomationEnabled()) {
    return;
  }

  uint32_t stage = g_automation_stage.load(std::memory_order_relaxed);
  while (stage < 2) {
    if (g_automation_stage.compare_exchange_weak(stage, 2, std::memory_order_relaxed)) {
      rex::kernel::xam::SetSyntheticAutoTap(rex::input::X_INPUT_GAMEPAD_START, true);
      REXLOG_INFO("Skate 3 demo path: enabled press-start auto-tap ({})", reason);
      return;
    }
  }
}

void EnableIntroMovieSkipIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }

  bool expected = false;
  if (g_skip_intro_movie.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    REXLOG_INFO("Skate 3 demo path: enabled intro movie completion override");
  }
}

void PollF10Marker() {
#if defined(_WIN32)
  if (!ProbeEnabled()) {
    return;
  }

  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool was_down = g_f10_poll_was_down.exchange(f10_down, std::memory_order_relaxed);
  if (f10_down && !was_down) {
    REXLOG_WARN("Skate 3 demo path: polled F10 milestone marker");
  }
#endif
}

extern "C" REX_FUNC(Skate3DemoPath_SetFrontEndStateHook) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t state_id = ctx.r4.u32;
  const uint32_t mode = ctx.r5.u32;
  const uint32_t caller_lr = ctx.lr;

  if (ProbeEnabled()) {
    g_last_requested_state.store(state_id, std::memory_order_relaxed);
    REXLOG_INFO(
        "Skate 3 demo path: FE SetState state={} ({}) mode={} manager=0x{:08X} lr=0x{:08X}",
        state_id, KnownFrontEndStateName(state_id), mode, manager, caller_lr);
  }

  sub_82D0AFA0(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectStateHook) {
  PollF10Marker();
  LogBootFlowEventChange(g_last_language_select_event, "BootFlow LanguageSelectState",
                         ctx.r4.u32, ctx.r3.u32);
  if (ctx.r4.u32 == 4) {
    EnableIntroMovieSkipIfNeeded();
  }

  sub_826FDD70(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_ShowPressStartModeHook) {
  PollF10Marker();
  LogBootFlowEventChange(g_last_press_start_event, "BootFlow ShowPressStartMode", ctx.r4.u32,
                         ctx.r3.u32);
  if (ctx.r4.u32 == 1) {
    g_skip_intro_movie.store(false, std::memory_order_relaxed);
    EnableTitleStartAutoTapIfNeeded("press-start state");
  }

  sub_826FE1D8(ctx, base);
}

extern "C" REX_FUNC(Skate3DemoPath_LanguageSelectUpdateHook) {
  PollF10Marker();
  if (ProbeEnabled()) {
    bool expected = false;
    if (g_seen_language_update.compare_exchange_strong(expected, true,
                                                       std::memory_order_relaxed)) {
      REXLOG_INFO(
          "Skate 3 demo path: FrontEndState_LanguageSelect::OnUpdate first seen "
          "dt={} this=0x{:08X}",
          ctx.r4.u32, ctx.r3.u32);
      QueueLanguageAcceptIfNeeded();
    }
  }

  sub_82639400(ctx, base);
}

// Scripted post-gameplay input sequence (e.g. driving the pause-menu map
// switch that reproduces the sticky-slow-GPU state). Runs on its own thread
// with real-time pacing: waits for the gameplay presence context, lets the
// scene settle, then injects one pad input at a time.
struct GameplayInputToken {
  const char* name;
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
};

constexpr GameplayInputToken kGameplayInputTokens[] = {
    {"a", rex::input::X_INPUT_GAMEPAD_A, 0, 0},
    {"b", rex::input::X_INPUT_GAMEPAD_B, 0, 0},
    {"x", rex::input::X_INPUT_GAMEPAD_X, 0, 0},
    {"y", rex::input::X_INPUT_GAMEPAD_Y, 0, 0},
    {"start", rex::input::X_INPUT_GAMEPAD_START, 0, 0},
    {"back", rex::input::X_INPUT_GAMEPAD_BACK, 0, 0},
    {"lb", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0},
    {"rb", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0},
    {"lt", 0, 255, 0},
    {"rt", 0, 0, 255},
    {"up", rex::input::X_INPUT_GAMEPAD_DPAD_UP, 0, 0},
    {"down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, 0, 0},
    {"left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, 0, 0},
    {"right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, 0, 0},
    {"l3", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, 0, 0},
    {"r3", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, 0, 0},
};

const GameplayInputToken* FindGameplayInputToken(const std::string& name) {
  for (const GameplayInputToken& token : kGameplayInputTokens) {
    if (name == token.name) {
      return &token;
    }
  }
  return nullptr;
}

// Set once during app setup (before the guest boots), read from the guest
// thread by the movie-skip poll.
std::function<rex::input::InputSystem*()> g_ui_input_provider;

// User-facing movie skip: a fresh A / Start press while a frontend movie is
// updating completes it (the generated FEMoviePlayer::Update patch consults
// ShouldForceIntroMovieComplete every movie tick, demo path or not). The
// merged raw pad state is polled so the default keyboard bindings (Space = A,
// Enter = Start) work identically on every backend; rising-edge only, so a
// button held since before the movie doesn't blow straight through it.
bool UserRequestedMovieSkip() {
  static std::atomic<bool> s_was_down{true};
  if (!REXCVAR_GET(skate3_intro_movie_skip) || !g_ui_input_provider) {
    return false;
  }
  rex::input::InputSystem* input = g_ui_input_provider();
  if (input == nullptr) {
    return false;
  }
  constexpr uint16_t kSkipButtons =
      rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_START;
  rex::input::X_INPUT_GAMEPAD pad;
  const bool down =
      input->GetUiGamepadState(&pad) && (pad.buttons & kSkipButtons) != 0;
  const bool was_down = s_was_down.exchange(down, std::memory_order_relaxed);
  if (down && !was_down) {
    REXLOG_INFO("Skate 3: frontend movie skipped by user input");
    return true;
  }
  return false;
}

std::atomic<bool> g_input_worker_quit{false};
std::thread g_input_worker;

void JoinGameplayInputWorker() {
  g_input_worker_quit.store(true, std::memory_order_relaxed);
  if (g_input_worker.joinable()) {
    g_input_worker.join();
  }
}

bool InterruptibleSleepMs(int64_t total_ms) {
  constexpr int64_t kSliceMs = 100;
  while (total_ms > 0) {
    if (g_input_worker_quit.load(std::memory_order_relaxed)) {
      return false;
    }
    int64_t slice = total_ms < kSliceMs ? total_ms : kSliceMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    total_ms -= slice;
  }
  return !g_input_worker_quit.load(std::memory_order_relaxed);
}

void StartGameplayInputWorkerIfNeeded() {
  if (!AutomationEnabled()) {
    return;
  }
  const std::string sequence = REXCVAR_GET(skate3_demo_path_gameplay_inputs);
  if (sequence.empty()) {
    return;
  }

  const int32_t settle_ms = REXCVAR_GET(skate3_demo_path_input_settle_ms);
  const int32_t default_delay_ms = REXCVAR_GET(skate3_demo_path_input_delay_ms);

  struct SequenceEntry {
    const GameplayInputToken* token;
    int32_t delay_after_ms;
  };
  std::vector<SequenceEntry> tokens;
  std::stringstream stream(sequence);
  std::string raw_token;
  while (std::getline(stream, raw_token, ',')) {
    // Allow spaces around tokens.
    size_t begin = raw_token.find_first_not_of(" \t");
    size_t end = raw_token.find_last_not_of(" \t");
    if (begin == std::string::npos) {
      continue;
    }
    std::string name = raw_token.substr(begin, end - begin + 1);
    // Optional ':ms' suffix overriding the delay after this input (slower UI
    // transitions - the map-menu RT tabs - need more time to register).
    int32_t delay_after_ms = default_delay_ms;
    if (size_t colon = name.find(':'); colon != std::string::npos) {
      delay_after_ms = std::atoi(name.c_str() + colon + 1);
      if (delay_after_ms <= 0) {
        REXLOG_WARN("Skate 3 demo path: bad delay in gameplay input token '{}' - sequence "
                    "disabled",
                    name);
        return;
      }
      name.resize(colon);
    }
    const GameplayInputToken* token = FindGameplayInputToken(name);
    if (!token) {
      REXLOG_WARN("Skate 3 demo path: unknown gameplay input token '{}' - sequence disabled",
                  name);
      return;
    }
    tokens.push_back({token, delay_after_ms});
  }
  if (tokens.empty()) {
    return;
  }
  g_input_worker = std::thread([tokens, settle_ms] {
    // Wait for the gameplay presence context (0x8001 == 1).
    while (rex::kernel::guest_presence::GameplayContextValue() != 1) {
      if (!InterruptibleSleepMs(100)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay reached; injecting {} inputs after {} ms settle",
                tokens.size(), settle_ms);
    if (!InterruptibleSleepMs(settle_ms)) {
      return;
    }
    for (size_t i = 0; i < tokens.size(); ++i) {
      const GameplayInputToken* token = tokens[i].token;
      // ~8 polls at the game's 60 Hz input tick = a ~130 ms press. Triggers
      // are analog and debounced more heavily by the game's menus - hold them
      // for a human-tap-length ~270 ms.
      const bool is_trigger = token->left_trigger != 0 || token->right_trigger != 0;
      rex::kernel::xam::QueueSyntheticInput(token->buttons, token->left_trigger,
                                            token->right_trigger, is_trigger ? 16 : 8);
      REXLOG_INFO("Skate 3 demo path: injected gameplay input {}/{} '{}' (delay {} ms)", i + 1,
                  tokens.size(), token->name, tokens[i].delay_after_ms);
      if (!InterruptibleSleepMs(tokens[i].delay_after_ms)) {
        return;
      }
    }
    REXLOG_INFO("Skate 3 demo path: gameplay input sequence complete");
  });
  std::atexit(JoinGameplayInputWorker);
}

}  // namespace

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher || !ProbeEnabled()) {
    return;
  }

  dispatcher->SetFunction(0x82D0AFA0, &Skate3DemoPath_SetFrontEndStateHook);
  dispatcher->SetFunction(0x826FDD70, &Skate3DemoPath_LanguageSelectStateHook);
  dispatcher->SetFunction(0x826FE1D8, &Skate3DemoPath_ShowPressStartModeHook);
  dispatcher->SetFunction(0x82639400, &Skate3DemoPath_LanguageSelectUpdateHook);
  REXLOG_INFO("Skate 3 demo path: frontend probe hooks installed");
  StartGameplayInputWorkerIfNeeded();
}

rex::input::InputSystem* GetUiInputSystem() {
  return g_ui_input_provider ? g_ui_input_provider() : nullptr;
}

void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider) {
  g_ui_input_provider = std::move(provider);
}

bool ShouldForceIntroMovieComplete() {
  if (AutomationEnabled() && g_skip_intro_movie.load(std::memory_order_relaxed)) {
    bool expected = false;
    if (g_logged_intro_movie_skip.compare_exchange_strong(expected, true,
                                                          std::memory_order_relaxed)) {
      REXLOG_INFO("Skate 3 demo path: forcing frontend intro movie complete");
    }
    return true;
  }
  return UserRequestedMovieSkip();
}

}  // namespace skate3::demo_path
