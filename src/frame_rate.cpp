// The menus at up to 60 frames a second: the game presents every second
// vblank (30), which its matches need on the PS5 (their frames take over
// 16.7 ms), but its menus don't.

#include <rex/cvar.h>
#include <rex/hook.h>

#include "dualsense.h"

REXCVAR_DEFINE_BOOL(menus_60fps, true, "GRFS",
                    "The menus at up to 60 frames a second (matches stay at 30)");

REX_EXTERN(__imp__sub_8267C998);

// D3D swap scheduling, run for each presented frame. r3 packs the frontbuffer
// address (bits 12-31), the presentation interval in vblanks (bits 8-11) and a
// late-frame threshold (bits 0-7); the game asks for 2.
REX_HOOK_RAW(sub_8267C998) {
  uint32_t interval = (ctx.r3.u32 >> 8) & 0xF;
  if (REXCVAR_GET(menus_60fps) && InMenus() && interval > 1) {
    ctx.r3.u64 = (ctx.r3.u32 & ~UINT32_C(0xF00)) | (UINT32_C(1) << 8);
  }
  __imp__sub_8267C998(ctx, base);
}
