// See dualsense.h. The play state comes from the Flash screens' commands:
// "Launch" starts a match (or a mission), "Grid" over 0 is the pause (and the
// tactical map), "SpawnScreenOpened" the multiplayer's loadout choice until
// "respawn", and a menu screen loaded ("ScreenLoaded X.feu") ends it.

#include "dualsense.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#if defined(__PROSPERO__)
extern "C" {
struct SceUserServiceLoginUserIdList {
  int32_t user_id[4];
};
int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList* list);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
struct ScePadLightBarParam {
  uint8_t r, g, b, padding;
};
int scePadSetLightBar(int32_t handle, const ScePadLightBarParam* param);
// Adaptive triggers: a mask of the triggers set (L2 1, R2 2) and a command
// for each (L2 first): its mode, then its parameters.
struct ScePadTriggerEffectCommand {
  uint32_t mode;  // 0 off, 1 feedback, 2 weapon, 3 vibration
  uint8_t padding[4];
  uint8_t data[48];
};
struct ScePadTriggerEffectParam {
  uint8_t trigger_mask;
  uint8_t padding[7];
  ScePadTriggerEffectCommand command[2];
};
int scePadSetTriggerEffect(int32_t handle, const ScePadTriggerEffectParam* param);
}
#endif

namespace {

enum class State { kMenu, kPlay, kPaused };

std::atomic<State> state{State::kMenu};
// Until when the light bar flashes red (steady clock, ms).
std::atomic<int64_t> flash_until{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(__PROSPERO__)
// Keeps every logged-in user's DualSense in step with the state, 20 times a
// second (a pad connected later gets it too).
void Run() {
  int32_t handles[4] = {-1, -1, -1, -1};
  State applied[4] = {};
  bool triggers_set[4] = {};
  uint32_t colors[4] = {};
  bool logged = false;
  for (int64_t tick = 0;; ++tick) {
    if (tick % 20 == 0) {
      SceUserServiceLoginUserIdList users{{-1, -1, -1, -1}};
      sceUserServiceGetLoginUserIdList(&users);
      for (int i = 0; i < 4; ++i) {
        int32_t handle = users.user_id[i] == -1 || users.user_id[i] == 0xFF
                             ? -1
                             : scePadGetHandle(users.user_id[i], 0, 0);
        if (handle != handles[i]) {
          handles[i] = handle;
          triggers_set[i] = false;
          colors[i] = 0;
        }
      }
    }
    const State now = state;
    const bool flashing = NowMs() < flash_until;
    uint32_t color = flashing ? 0xFF1A1A
                     : now == State::kPlay ? 0x00A0FF
                     : now == State::kPaused ? 0x002850
                                             : 0x0050C8;
    for (int i = 0; i < 4; ++i) {
      if (handles[i] < 0) {
        continue;
      }
      if (color != colors[i]) {
        colors[i] = color;
        ScePadLightBarParam light{uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color), 0};
        scePadSetLightBar(handles[i], &light);
      }
      if (!triggers_set[i] || applied[i] != now) {
        triggers_set[i] = true;
        applied[i] = now;
        ScePadTriggerEffectParam effect;
        std::memset(&effect, 0, sizeof(effect));
        effect.trigger_mask = 0x03;
        if (now == State::kPlay) {
          // L2: a light resistance from a third of the way (position 3 of
          // 0-9, strength 2 of 1-8); R2: a gun's trigger, resisting from 2
          // and giving way at 6, firmly (6 of 1-8).
          effect.command[0].mode = 1;
          effect.command[0].data[0] = 3;
          effect.command[0].data[1] = 2;
          effect.command[1].mode = 2;
          effect.command[1].data[0] = 2;
          effect.command[1].data[1] = 6;
          effect.command[1].data[2] = 6;
        }
        int result = scePadSetTriggerEffect(handles[i], &effect);
        if (!logged) {
          logged = true;
          REXLOG_INFO("DualSense: triggers {:#x}", uint32_t(result));
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
#endif

void Start() {
#if defined(__PROSPERO__)
  static std::once_flag started;
  std::call_once(started, [] { std::thread(Run).detach(); });
#endif
}

}  // namespace

void DualSenseUiCommand(const std::string& command, const std::string& value) {
  Start();
  if (command == "Launch" || command == "respawn") {
    state = State::kPlay;
  } else if (command == "SpawnScreenOpened") {
    state = State::kPaused;
  } else if (command == "Grid" && state != State::kMenu) {
    state = std::atof(value.c_str()) > 0.0 ? State::kPaused : State::kPlay;
  } else if (command == "ScreenLoaded" && value.size() > 4 &&
             value.compare(value.size() - 4, 4, ".feu") == 0) {
    state = State::kMenu;
  }
}

bool InMenus() {
  return state == State::kMenu;
}

REX_EXTERN(__imp__sub_829A6F58);

// Sets a pad's rumble (r3 the user, r4 its two motors' speeds): a strong one
// (hits, explosions) flashes the light bar red.
REX_HOOK_RAW(sub_829A6F58) {
  uint32_t motors = ctx.r4.u32;
  if (motors) {
    uint16_t a = rex::memory::load_and_swap<uint16_t>(base + motors);
    uint16_t b = rex::memory::load_and_swap<uint16_t>(base + motors + 2);
    if (std::max(a, b) >= 0xC000 && state == State::kPlay) {
      flash_until = NowMs() + 300;
    }
  }
  __imp__sub_829A6F58(ctx, base);
}
