// System Link progression, the game executable's half (the AI module's is in
// src/ai_hooks.cpp, which defines the system_link_progression cvar).

#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>

namespace {

bool ProgressionEnabled() {
  static const bool enabled = rex::cvar::Query<bool>("system_link") &&
                              (!rex::cvar::GetFlagInfo("system_link_progression") ||
                               rex::cvar::Query<bool>("system_link_progression"));
  return enabled;
}

// Callers of the player state in the AI module's front end: the title screen's
// sign-in, and the main menu once it is shown.
constexpr uint32_t kTitleSignInCaller = 0x884F3208;
constexpr uint32_t kMainMenuCaller = 0x884F330C;

std::atomic<bool> past_title{false};

}  // namespace

REX_EXTERN(__imp__sub_822EDB48);

// The state of a player (r3: user index) as bits: 1 signed in, 2 signed in to
// LIVE, 4 logged in to the Ubisoft servers, ... The game only records XP and
// levels for players with bit 4, so in System Link signed-in players get it,
// past the title screen only: there, the game would wait for the servers.
REX_HOOK_RAW(sub_822EDB48) {
  uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_822EDB48(ctx, base);
  if (!ProgressionEnabled()) {
    return;
  }
  if (caller == kTitleSignInCaller) {
    past_title = false;
  } else if (caller == kMainMenuCaller) {
    past_title = true;
  }
  if (past_title && (ctx.r3.u32 & 1)) {
    ctx.r3.u64 = ctx.r3.u32 | 4;
  }
}

// Whether the player has a Uplay Passport (Ubisoft's online pass), asked by
// the AI module: the Uplay servers are gone, so every player has one, which
// lifts the level cap and the prompts to buy or link one.
REX_HOOK_RAW(sub_822F6630) {
  ctx.r3.u64 = 1;
}
