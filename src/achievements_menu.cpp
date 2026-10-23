// The main menu's UPLAY entry, useless without Uplay's servers, becomes
// TROPHIES: its label is renamed and selecting it opens the trophies screen
// (the achievements, PS5 style) instead of asking to sign in to Xbox LIVE.

#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/flags.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "connection_menu.h"
#include "grfs_app.h"

namespace {

// The handler of the commands the Flash screens send the game, in the AI
// module (reloaded on starts, so it's replaced again whenever it comes back).
// r4 is the argument count, r5 points to guest pointers to the strings, the
// first being the command.
constexpr uint32_t kUiCommandHandler = 0x884EEA08;
PPCFunc* ui_command_original = nullptr;

const char* GuestArgument(uint8_t* base, uint32_t argv, uint32_t index) {
  uint32_t address = rex::memory::load_and_swap<uint32_t>(base + argv + index * 4);
  return address ? reinterpret_cast<const char*>(base + address) : nullptr;
}

// Main.feu's ActionScript constant pool has the entry as a literal label,
// followed by its description string ID, its action and the next (beta-only)
// entry. Strings are referenced by index, so the pool keeps its size and its
// string count: the beta-only action name gives the longer label its room
// (consistently, as all its uses refer to the same string).
constexpr char kPoolUplay[] = "> UPLAY\0" "118632\0" "gouPlay\0" "117926\0" "goExitBeta";

std::string RenamedPool() {
  // XLanguage 9 is Portuguese.
  std::string label = REXCVAR_GET(user_language) == 9 ? "> TROFÉUS" : "> TROPHIES";
  std::string pool = label + std::string("\0" "118632\0" "gouPlay\0" "117926\0", 23);
  size_t room = sizeof(kPoolUplay) - 1 - pool.size();
  return pool + std::string("xBeta____").substr(0, room);
}

// Main.feu's goDLC (DOWNLOADABLE CONTENT, renamed in the Language files)
// opens its submenu with Xbox LIVE, else asks to sign in to it; the Ghost
// Feed's MULTIPLAYER CHALLENGES (renamed DONORS) opens its screen with Xbox
// LIVE too. Their functions (DefineFunction2 actions) start sending the game
// a command instead, which opens the "help keep me active" screen or the
// donors one.
struct Rewrite {
  std::string_view name;
  std::string_view command;
};
constexpr Rewrite kRewrites[] = {{"goDLC", "goDLC"}, {"onBtnMPChallengesActivate", "goDonors"}};

// push "Main", command, "Send", 3; fscommand2; pop; return undefined.
std::string SendBody(std::string_view command) {
  std::string push = std::string("\0Main\0", 6) + '\0' + std::string(command) + '\0' +
                     std::string("\0Send\0\x07\x03\0\0\0", 11);
  return std::string{'\x96', char(push.size()), '\0'} + push +
         std::string("\x2d\x17\x96\x01\x00\x03\x3e", 7);
}

// The DefineFunction2 action at data[at] (0x8e, u16 size, the name, u16
// parameter count, u8 registers, u16 flags, the parameters (u8 register, a
// name), u16 code size, the code) starts with body.
void RewriteFunction(char* data, size_t size, size_t at, std::string_view body) {
  size_t p = at + 3;
  p += strnlen(data + p, size - p) + 1;
  if (p + 5 > size) {
    return;
  }
  size_t parameters = uint8_t(data[p]) | uint8_t(data[p + 1]) << 8;
  p += 5;
  for (size_t i = 0; i < parameters && p < size; ++i) {
    p += 1 + strnlen(data + p + 1, size - p - 1) + 1;
  }
  if (p + 2 > size) {
    return;
  }
  size_t code_size = uint8_t(data[p]) | uint8_t(data[p + 1]) << 8;
  p += 2;
  if (code_size >= body.size() && p + code_size <= size) {
    std::memcpy(data + p, body.data(), body.size());
  }
}

void RenameMainMenuEntry(rex::memory::Memory* memory) {
  static const std::string renamed = RenamedPool();
  static_assert(sizeof(kPoolUplay) - 1 == 40);
  const std::string_view pattern(kPoolUplay, sizeof(kPoolUplay) - 1);
  int count = 0;
  // The screens are loaded to the 4 KB and 64 KB page virtual heaps.
  for (uint32_t heap_address : {0x00000000u, 0x40000000u}) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(heap_address);
    if (!heap) {
      continue;
    }
    uint32_t address = heap->heap_base();
    const uint32_t end = heap->heap_base() + heap->heap_size();
    while (address < end) {
      rex::memory::HeapAllocationInfo info;
      if (!heap->QueryRegionInfo(address, &info)) {
        break;
      }
      if (!info.region_size) {
        address += heap->page_size();
        continue;
      }
      if ((info.state & rex::memory::kMemoryAllocationCommit) &&
          (info.protect & rex::memory::kMemoryProtectWrite)) {
        char* data = memory->TranslateVirtual<char*>(info.base_address);
        std::string_view region(data, info.region_size);
        for (size_t at = region.find(pattern); at != std::string_view::npos;
             at = region.find(pattern, at + pattern.size())) {
          std::memcpy(data + at, renamed.data(), renamed.size());
          ++count;
        }
        for (const Rewrite& rewrite : kRewrites) {
          const std::string body = SendBody(rewrite.command);
          std::string name = std::string(rewrite.name) + '\0';
          for (size_t at = region.find(name); at != std::string_view::npos;
               at = region.find(name, at + 1)) {
            // (the action's code and u16 size before the name)
            if (at >= 3 && uint8_t(data[at - 3]) == 0x8e) {
              RewriteFunction(data, info.region_size, at - 3, body);
            }
          }
        }
      }
      address = info.base_address + info.region_size;
    }
  }
  if (count) {
    REXLOG_INFO("Main menu: UPLAY renamed to trophies ({} copies)", count);
  }
}

void UiCommand(PPCContext& ctx, uint8_t* base) {
  uint32_t argc = ctx.r4.u32, argv = ctx.r5.u32;
  const char* command = argc && argv ? GuestArgument(base, argv, 0) : nullptr;
  if (command) {
    if (!std::strcmp(command, "uPlayPressed")) {
      if (GrfsApp* app = GrfsApp::instance()) {
        app->ShowTrophies();
      }
      return;
    }
    // DOWNLOADABLE CONTENT and MULTIPLAYER CHALLENGES (their texts renamed in
    // the Language files) open the "help keep me active" and donors screens.
    if (!std::strcmp(command, "goDLC") || !std::strcmp(command, "goDonors")) {
      if (GrfsApp* app = GrfsApp::instance()) {
        if (!std::strcmp(command, "goDLC")) {
          app->ShowSupport();
        } else {
          app->ShowDonors();
        }
      }
      return;
    }
    if (!std::strcmp(command, "ScreenLoaded")) {
      RenameMainMenuEntry(rex::system::kernel_state()->memory());
    } else if (!std::strcmp(command, "RequestChangeConnectionType") && argc > 1) {
      uint32_t type = rex::memory::load_and_swap<uint32_t>(base + argv + 4);
      if (type) {
        OnConnectionRequest(base, type);
      }
    }
  }
  ui_command_original(ctx, base);
}

}  // namespace

REX_EXTERN(__imp__sub_822E45D8);

// Delivers the queued Flash commands to their handlers, so it runs before the
// handler in the AI module is ever called.
REX_HOOK_RAW(sub_822E45D8) {
  auto* dispatcher = rex::system::kernel_state()->function_dispatcher();
  PPCFunc* handler = dispatcher->GetFunction(kUiCommandHandler);
  if (handler && handler != &UiCommand) {
    ui_command_original = handler;
    dispatcher->SetFunction(kUiCommandHandler, &UiCommand);
  }
  __imp__sub_822E45D8(ctx, base);
}
