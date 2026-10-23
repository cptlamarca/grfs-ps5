// Patches to the AI module (AICLASS_X360_F.xex, where the game logic lives),
// built into that module so they override its weak recompiled functions.

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(system_link_progression, true, "GRFS",
                    "Class levels and item unlocks in System Link, as online: without it, "
                    "System Link games have every item unlocked");

REX_EXTERN(__imp__sub_881AA208);

// Whether the session ranks players (levels, XP, locked items): only online
// sessions do, so the gunsmith and loadouts let anything be picked offline.
REX_HOOK_RAW(sub_881AA208) {
  __imp__sub_881AA208(ctx, base);
  if (REXCVAR_GET(system_link_progression)) {
    ctx.r3.u64 = 1;
  }
}
