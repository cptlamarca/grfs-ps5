// The multiplayer menu's CONNECTION choices: PlayStation Network (Xbox LIVE,
// gone) becomes "Local Network", System Link on the LAN, and System Link
// becomes "Online (VPS)", System Link through the relay (system_link_relay).
// Their texts are 114484 (Custom Match, an Xbox LIVE type) and 116215, set in
// the Language files.
//
// Multiplayer.feu's ActionScript lists them: without Xbox LIVE, the first is
// {name: text 107837, enum: undefined}, which asks to sign in; the second
// {name: connectionTypes[LAN], enum: LAN}, which sends the game
// RequestChangeConnectionType with the LAN's type (3). So, in its loaded
// bytecode (same sizes: its jumps stay right), the first gets text 114484
// and that text ID as its type, which the game gets as a LAN request and the
// relay off. The CONNECTION label shows connectionTypes[LAN]; on the LAN, it
// shows connectionTypes[a private Xbox LIVE game], text 114484.

#include "connection_menu.h"

#include "achievements_ui.h"
#include "grfs_app.h"
#include "player_account.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/hook.h>
#include <rex/kernel/xam/system_link_relay.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {

namespace relay = rex::kernel::xam::system_link_relay;

constexpr char kLanType[] = "3";
constexpr char kLocalText[] = "114484";
// Multiplayer.feu's constant pool, around the first entry's text IDs (one for
// the PS3, one for others).
constexpr std::string_view kPoolTexts("107837\0" "107838\0" "connectionTypes", 29);
constexpr std::string_view kPatchedTexts("114484\0" "114484\0" "connectionTypes", 29);

std::mutex labels_mutex;
bool on_lan = false;
// The CONNECTION labels' connectionTypes key bytes: the LAN's and the private
// game's constant pool indices.
struct LabelKey {
  uint32_t address;
  uint8_t lan, local;
};
std::vector<LabelKey> label_keys;

// The ActionScript 2 push of a constant pool string.
std::string PushString(uint32_t index) {
  if (index < 256) {
    return {'\x08', char(index)};
  }
  return {'\x09', char(index & 0xFF), char(index >> 8)};
}

// The strings of the ConstantPool action (0x88: u16 size, u16 count, strings)
// holding data[at]; the action's end goes to pool_end.
bool FindPool(const char* data, size_t size, size_t at, std::map<std::string, uint32_t>& indices,
              size_t& pool_end) {
  for (size_t start = at; start-- > 0 && at - start < 0x20000;) {
    if (uint8_t(data[start]) != 0x88 || start + 5 > size) {
      continue;
    }
    size_t length = uint8_t(data[start + 1]) | uint8_t(data[start + 2]) << 8;
    size_t count = uint8_t(data[start + 3]) | uint8_t(data[start + 4]) << 8;
    size_t end = start + 3 + length;
    if (end <= at || end > size || count < 16) {
      continue;
    }
    std::map<std::string, uint32_t> found;
    size_t p = start + 5;
    uint32_t i = 0;
    for (; i < count && p < end; ++i) {
      size_t n = strnlen(data + p, end - p);
      found.emplace(std::string(data + p, n), i);
      p += n + 1;
    }
    if (i == count && p == end) {
      indices = std::move(found);
      pool_end = end;
      return true;
    }
  }
  return false;
}

// bytes in data[0, size), from their second byte on (the first, a 0, is
// common): memchr for it, then the rest (the PS5's libc has no memmem).
char* FindBytes(char* data, size_t size, std::string_view bytes) {
  for (char* end = data + size; size_t(end - data) >= bytes.size(); ++data) {
    data = static_cast<char*>(std::memchr(data + 1, bytes[1], end - data - bytes.size() + 1));
    if (!data) {
      return nullptr;
    }
    --data;
    if (!std::memcmp(data, bytes.data(), bytes.size())) {
      return data;
    }
  }
  return nullptr;
}

void PatchScreen(char* data, size_t size, uint32_t guest_base, size_t at) {
  std::map<std::string, uint32_t> pool;
  size_t pool_end;
  if (!FindPool(data, size, at, pool, pool_end)) {
    return;
  }
  for (const char* name : {"getText", "enum", "name", "2", "connectionTypes", "EConnectionTypes",
                           "eGameTypeLANGame", "eGameTypePrivateXboxLiveGame"}) {
    if (!pool.count(name)) {
      return;
    }
  }
  std::memcpy(data + at, kPatchedTexts.data(), kPatchedTexts.size());
  std::string_view code(data + pool_end, std::min<size_t>(size - pool_end, 0x10000));

  // {name: text, enum: undefined} -> {name: text, enum: text}: the push of
  // "enum", undefined, 2 (int) becomes "enum", the text's register, "2" (a
  // string), and toNumber.
  uint32_t enum_index = pool["enum"];
  std::string item = PushString(pool["getText"]) + std::string("\x52\x96\x08\x00", 4) + PushString(enum_index) +
                     std::string("\x03\x07\x02\x00\x00\x00\x43", 7);
  std::string name_register = PushString(pool["name"]) + "\x04";
  size_t found = code.find(item);
  size_t named = found == std::string_view::npos ? found : code.rfind(name_register, found);
  if (enum_index < 256 && named != std::string_view::npos && found - named < 64) {
    char reg = code[named + name_register.size()];
    std::string body = PushString(enum_index) + std::string{'\x04', reg} + PushString(pool["2"]);
    std::string patched = std::string{'\x96', char(body.size()), '\0'} + body;
    patched += std::string(item.size() - PushString(pool["getText"]).size() - 1 - patched.size() - 1,
                           '\x4a') +
               '\x43';
    std::memcpy(data + pool_end + found + PushString(pool["getText"]).size() + 1, patched.data(),
                patched.size());
  }

  // A choice of the connection type the game already has (the LAN, for both)
  // isn't sent, nor its update shown: their checks compare the type (r2) with
  // gamerData.CurrentAdversarialConnectionTypeIndex, which becomes
  // gamerData.enum (undefined, so never equal).
  if (pool.count("gamerData") && pool.count("CurrentAdversarialConnectionTypeIndex")) {
    std::string current = PushString(pool["CurrentAdversarialConnectionTypeIndex"]);
    std::string check = std::string("\x87\x01\x00\x02\x17\x96\x06\x00\x04\x02\x04\x01", 12) +
                        PushString(pool["gamerData"]) + "\x4e\x96" + char(current.size()) +
                        std::string(1, '\0') + current + "\x4e\x49\x12\x12\x9d";
    std::string other = current.size() == 2 ? PushString(enum_index)
                                            : std::string{'\x09', char(enum_index & 0xFF),
                                                          char(enum_index >> 8)};
    if (other.size() == current.size()) {
      for (size_t p = code.find(check); p != std::string_view::npos; p = code.find(check, p + 1)) {
        std::memcpy(data + pool_end + p + check.size() - 5 - current.size(), other.data(),
                    other.size());
      }
    }
  }

  // The CONNECTION labels: connectionTypes[EConnectionTypes.eGameTypeLANGame].
  uint32_t lan = pool["eGameTypeLANGame"], local = pool["eGameTypePrivateXboxLiveGame"];
  if (lan < 256 && local < 256) {
    // (either key: the screen may be patched already)
    std::string label = PushString(pool["connectionTypes"]) +
                        std::string("\x4e\x96\x04\x00\x04\x01", 6) +
                        PushString(pool["EConnectionTypes"]) +
                        std::string("\x4e\x96\x02\x00\x08", 5);
    std::lock_guard<std::mutex> lock(labels_mutex);
    for (size_t p = code.find(label); p != std::string_view::npos; p = code.find(label, p + 1)) {
      size_t key = p + label.size();
      if (key + 4 > code.size() || (uint8_t(code[key]) != lan && uint8_t(code[key]) != local) ||
          code.substr(key + 1, 3) != "\x4e\x4e\x47") {
        continue;
      }
      label_keys.push_back({guest_base + uint32_t(pool_end + key), uint8_t(lan), uint8_t(local)});
      data[pool_end + key] = char(on_lan ? local : lan);
    }
  }
}

// Under labels_mutex.
void SetLabels(uint8_t* base) {
  for (const LabelKey& key : label_keys) {
    uint8_t& byte = base[key.address];
    if (byte == key.lan || byte == key.local) {
      byte = on_lan ? key.local : key.lan;
    }
  }
}

}  // namespace

void PatchConnectionMenu(rex::memory::Memory* memory) {
  {
    std::lock_guard<std::mutex> lock(labels_mutex);
    label_keys.clear();
  }
  int count = 0;
  bool done = false;
  for (uint32_t heap_address : {0x00000000u, 0x40000000u}) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(heap_address);
    if (!heap) {
      continue;
    }
    uint32_t address = heap->heap_base();
    const uint32_t end = heap->heap_base() + heap->heap_size();
    while (address < end && !done) {
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
        const size_t size = info.region_size;
        // One pass (fast) for their common end, then their starts.
        constexpr std::string_view kEnd("\0connectionTypes", 16);
        const size_t start = kPoolTexts.size() - kEnd.size();
        for (char* hit = FindBytes(data, size, kEnd); hit && !done;
             hit = FindBytes(hit + 1, data + size - hit - 1, kEnd)) {
          size_t at = size_t(hit - data);
          if (at < start) {
            continue;
          }
          at -= start;
          const bool fresh = !std::memcmp(data + at, kPoolTexts.data(), start);
          if (fresh || !std::memcmp(data + at, kPatchedTexts.data(), start)) {
            PatchScreen(data, size, info.base_address, at);
            ++count;
            // The screen just loaded is the one copy not patched yet: once
            // it is, the rest of the heaps (older copies) can wait.
            done = fresh;
          }
        }
      }
      address = info.base_address + info.region_size;
    }
  }
  if (count) {
    REXLOG_INFO("Multiplayer: connection choices patched ({} copies)", count);
  }
}

void OnConnectionRequest(uint8_t* base, uint32_t type_address) {
  char* type = reinterpret_cast<char*>(base + type_address);
  if (!std::strcmp(type, kLocalText)) {
    // (as long as its text ID)
    std::memcpy(type, kLanType, sizeof(kLanType));
  }
}

REX_EXTERN(__imp__sub_822E4B18);

// Queues a command from a Flash screen (r5 arguments, r6 pointing to guest
// pointers to UTF-16 strings) while its ActionScript runs. Multiplayer.feu
// sends ScreenLoaded before it lists the connections, so they're patched
// then, before that (the queued command comes too late).
REX_HOOK_RAW(sub_822E4B18) {
  uint32_t argc = ctx.r5.u32 & 0xFFFF, argv = ctx.r6.u32;
  auto argument = [&](uint32_t index) {
    std::string text;
    uint32_t address = rex::memory::load_and_swap<uint32_t>(base + argv + 4 * index);
    for (uint16_t c; address && (c = rex::memory::load_and_swap<uint16_t>(base + address)) &&
                     text.size() < 64;
         address += 2) {
      text += char(c);
    }
    return text;
  };
  std::string command = argc > 1 ? argument(0) : std::string();
  std::string value = argc > 1 ? argument(1) : std::string();
  // A connection choice switches the relay and the labels now: the screen
  // shows its CONNECTION label again before the queued command comes.
  bool online = false;
  if (command == "RequestChangeConnectionType" && (value == kLocalText || value == kLanType)) {
    bool lan = value == kLocalText;
    relay::SetActive(!lan);
    std::lock_guard<std::mutex> lock(labels_mutex);
    on_lan = lan;
    SetLabels(base);
    online = !lan;
  }
  __imp__sub_822E4B18(ctx, base);
  if (command == "ScreenLoaded" && value == "Multiplayer.feu") {
    PatchConnectionMenu(rex::system::kernel_state()->memory());
    std::lock_guard<std::mutex> lock(labels_mutex);
    online = !on_lan;
  }
  // A console banned from the online is told so when it would play there.
  std::string reason;
  if (online && PlayerBanned(&reason)) {
    if (GrfsApp* app = GrfsApp::instance()) {
      bool portuguese = IsPortuguese();
      std::string text = portuguese ? "Este console foi banido das partidas Online (VPS)."
                                    : "This console is banned from Online (VPS) matches.";
      if (!reason.empty()) {
        text += std::string(portuguese ? "\n\nMotivo: " : "\n\nReason: ") + reason;
      }
      text += portuguese ? "\n\nA Rede Local, a campanha e o Guerrilha continuam liberados."
                         : "\n\nLocal Network, the campaign and Guerrilla stay available.";
      app->ShowNotice(portuguese ? "Banido do online" : "Banned from the online", text);
    }
  }
}
