// Patches to the AI module (AICLASS_X360_F.xex, where the game logic lives),
// built into that module so they override its weak recompiled functions.

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

REXCVAR_DEFINE_BOOL(system_link_progression, true, "GRFS",
                    "Class levels and item unlocks in System Link, as online: without it, "
                    "System Link games have every item unlocked");

namespace {

// The player's progression: six classes (two factions of three) of 120 bytes
// from +51288, with their XP at +104 and rank at +111. Online, both come from
// the servers' stats. In System Link, the game puts the stats of a match that
// ends in its career variable, which loading the progression reads into the
// classes; resetting the progression clears them. So the XP of each match
// result is added up in a file of the profile, and the classes get their
// totals back, with the ranks they reach.
constexpr uint32_t kClasses = 51288, kClassSize = 120, kClassXp = 104, kClassRank = 111;
constexpr uint32_t kClassCount = 6;
// The game's settings: the number of ranks (u8 at +4690) and the XP each one
// takes from the start (u32 table at *(+4696)).
constexpr uint32_t kSettingsPointer = 0x887FDF58, kRankCount = 4690, kRankXpPointer = 4696;
// The game executable's variables: a table (at *kVariables) of u16 offsets
// into its data, one per variable at 2 * (id + 27648), 0xFFFF for variables
// that don't exist. The career is variable 12, its size variable 13.
constexpr uint32_t kVariables = 0x82EDE1A4, kCareer = 12, kCareerSize = 13;
constexpr char kXpFileName[] = "system_link_xp.bin";
constexpr char kXpMagic[4] = {'G', 'R', 'X', '2'};

struct XpFile {
  std::array<uint32_t, kClassCount> totals{};
  // The match result last added up (a hash of the career variable).
  uint64_t counted_career = 0;
};

std::mutex xp_mutex;
bool xp_loaded = false;
XpFile xp;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreGuest32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// The guest address of a game variable, 0 if it doesn't exist.
uint32_t VariableAddress(const uint8_t* base, uint32_t id) {
  uint32_t table = LoadGuest32(base, kVariables);
  if (!table) {
    return 0;
  }
  const uint8_t* entry = base + table + 2 * (id + 27648);
  uint32_t offset = entry[0] << 8 | entry[1];
  return offset == 0xFFFF ? 0 : table + offset;
}

// A hash (FNV-1a) of the career variable, 0 without one.
uint64_t CareerHash(const uint8_t* base) {
  uint32_t career = VariableAddress(base, kCareer), size = VariableAddress(base, kCareerSize);
  if (!career || !size) {
    return 0;
  }
  uint32_t length = std::min<uint32_t>(LoadGuest32(base, size), 0x2000);
  uint64_t hash = 0xCBF29CE484222325;
  for (uint32_t i = 0; i < length; ++i) {
    hash = (hash ^ base[career + i]) * 0x100000001B3;
  }
  return length ? hash : 0;
}

std::filesystem::path XpPath() {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() /
         kXpFileName;
}

// Under xp_mutex.
XpFile& Xp() {
  if (!xp_loaded) {
    xp_loaded = true;
    std::ifstream file(XpPath(), std::ios::binary);
    char magic[sizeof(kXpMagic)];
    XpFile loaded;
    if (file.read(magic, sizeof(magic)) && !std::memcmp(magic, kXpMagic, sizeof(magic)) &&
        file.read(reinterpret_cast<char*>(loaded.totals.data()), sizeof(loaded.totals)) &&
        file.read(reinterpret_cast<char*>(&loaded.counted_career), sizeof(loaded.counted_career))) {
      xp = loaded;
    }
  }
  return xp;
}

void SaveXp() {
  std::filesystem::path path = XpPath();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(kXpMagic, sizeof(kXpMagic));
  file.write(reinterpret_cast<const char*>(xp.totals.data()), sizeof(xp.totals));
  file.write(reinterpret_cast<const char*>(&xp.counted_career), sizeof(xp.counted_career));
}

// The rank of a class with that XP, before the game caps it.
uint32_t RankForXp(const uint8_t* base, uint32_t class_xp) {
  uint32_t settings = LoadGuest32(base, kSettingsPointer);
  if (!settings) {
    return 0;
  }
  uint32_t count = base[settings + kRankCount];
  uint32_t table = LoadGuest32(base, settings + kRankXpPointer);
  uint32_t rank = 0;
  while (table && rank + 1 < count && LoadGuest32(base, table + 4 * (rank + 1)) <= class_xp) {
    ++rank;
  }
  return rank;
}

}  // namespace

REX_EXTERN(__imp__sub_881AA208);
REX_EXTERN(__imp__sub_881719E0);
REX_EXTERN(__imp__sub_8818C5C0);

// Whether the player has a Uplay Passport (with the "UplayPassport" debug
// option, or from the game executable), and whether the progression may go
// past the level cap without one: the Uplay servers are gone, so always.
REX_HOOK_RAW(sub_881ABB68) {
  ctx.r3.u64 = 1;
}

REX_HOOK_RAW(sub_88174718) {
  ctx.r3.u64 = 1;
}

// Whether the session ranks players (levels, XP, locked items): only online
// sessions do, so the gunsmith and loadouts let anything be picked offline.
REX_HOOK_RAW(sub_881AA208) {
  __imp__sub_881AA208(ctx, base);
  if (REXCVAR_GET(system_link_progression)) {
    ctx.r3.u64 = 1;
  }
}

// Works out what the player's progression (r3) unlocks from the ranks of its
// classes, after the game resets or loads it. First, if the classes hold a
// match result not added up yet, the totals take it in; then each
// class gets its total XP and its rank, capped as the game does.
REX_HOOK_RAW(sub_881719E0) {
  if (REXCVAR_GET(system_link_progression)) {
    uint32_t progression = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(xp_mutex);
    XpFile& file = Xp();
    uint64_t career = CareerHash(base);
    bool loaded_result = false;
    for (uint32_t i = 0; i < kClassCount; ++i) {
      loaded_result |= LoadGuest32(base, progression + kClasses + i * kClassSize + kClassXp) != 0;
    }
    if (career && career != file.counted_career && loaded_result) {
      for (uint32_t i = 0; i < kClassCount; ++i) {
        // The game adds the XP of a match to what the class had then: its
        // total, as restored below, or nothing if the game had reset it.
        uint32_t loaded = LoadGuest32(base, progression + kClasses + i * kClassSize + kClassXp);
        uint32_t total = loaded >= file.totals[i] ? loaded : file.totals[i] + loaded;
        if (total != file.totals[i]) {
          REXKRNL_INFO("System Link progression: class {} earned {} XP", i,
                       total - file.totals[i]);
        }
        file.totals[i] = total;
      }
      file.counted_career = career;
      SaveXp();
    }
    for (uint32_t i = 0; i < kClassCount; ++i) {
      uint32_t row = progression + kClasses + i * kClassSize;
      StoreGuest32(base, row + kClassXp, file.totals[i]);
      // The game's highest rank for one (with the DLC owned; 5, its lowest,
      // without a Uplay Passport): sub_8818C5C0.
      uint32_t rank = RankForXp(base, file.totals[i]);
      ctx.r3.u64 = rank;
      __imp__sub_8818C5C0(ctx, base);
      base[row + kClassRank] = uint8_t(std::min(rank, ctx.r3.u32));
    }
    ctx.r3.u64 = progression;
  }
  __imp__sub_881719E0(ctx, base);
}
