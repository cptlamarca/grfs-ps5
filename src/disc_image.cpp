// See disc_image.h. XDVDFS: 2048 byte sectors from the game partition's
// start (where it is depends on the disc's format), its volume descriptor at
// sector 32 ("MICROSOFT*XBOX*MEDIA", then the root directory's sector and
// size). A directory is a binary tree of entries, each: the left and right
// entries' offsets in 4 byte units (u16, 0 none), the data's sector and size
// (u32), attributes (u8, 0x10 a directory), the name's length (u8) and the
// name; entries are 4 byte aligned and don't cross sectors. Little endian.

#include "disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>

namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// The game partition's start: an image of the partition only (as extract
// tools make), and the XGD3, XGD2 and XGD1 discs.
constexpr uint64_t kPartitions[] = {0, 0x2080000, 0xFD90000, 0x18300000};
constexpr uint8_t kAttributeDirectory = 0x10;

uint32_t Read32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool ReadAt(std::ifstream& in, uint64_t offset, void* data, size_t size) {
  in.clear();
  in.seekg(std::streamoff(offset));
  in.read(static_cast<char*>(data), std::streamsize(size));
  return in.gcount() == std::streamsize(size);
}

struct Lister {
  std::ifstream& in;
  uint64_t partition;
  uint64_t image_size;
  std::vector<DiscFile>& files;
  std::set<uint64_t> seen;  // directories read, against loops in a broken image

  bool Directory(uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
    if (size == 0) {
      return true;
    }
    uint64_t start = partition + sector * kSector;
    if (depth > 32 || size > (64u << 20) || start + size > image_size ||
        !seen.insert(start).second) {
      return false;
    }
    std::vector<uint8_t> data(size);
    if (!ReadAt(in, start, data.data(), size)) {
      return false;
    }
    // The tree's entries, from the root; 0xFFFF fills a sector's end.
    std::vector<uint32_t> pending = {0};
    std::set<uint32_t> visited;
    while (!pending.empty()) {
      uint32_t at = pending.back();
      pending.pop_back();
      if (at + 14 > size || !visited.insert(at).second) {
        continue;
      }
      const uint8_t* entry = data.data() + at;
      uint16_t left = Read16(entry), right = Read16(entry + 2);
      if (left == 0xFFFF && right == 0xFFFF) {
        continue;
      }
      uint32_t data_sector = Read32(entry + 4), data_size = Read32(entry + 8);
      uint8_t attributes = entry[12], name_length = entry[13];
      if (at + 14 + name_length > size || name_length == 0) {
        return false;
      }
      std::string name(reinterpret_cast<const char*>(entry + 14), name_length);
      if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
          name == "." || name == "..") {
        return false;
      }
      if (left) {
        pending.push_back(uint32_t(left) * 4);
      }
      if (right) {
        pending.push_back(uint32_t(right) * 4);
      }
      DiscFile file;
      file.path = prefix + name;
      file.offset = partition + data_sector * kSector;
      file.size = data_size;
      file.directory = attributes & kAttributeDirectory;
      if (!file.directory && file.offset + file.size > image_size) {
        return false;
      }
      files.push_back(file);
      if (file.directory && !Directory(data_sector, data_size, file.path + "/", depth + 1)) {
        return false;
      }
    }
    return true;
  }
};

bool SameName(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
         });
}

}  // namespace

bool ListDiscImage(const std::filesystem::path& image, std::vector<DiscFile>& files,
                   std::string& error) {
  files.clear();
  std::error_code ec;
  uint64_t image_size = std::filesystem::file_size(image, ec);
  std::ifstream in(image, std::ios::binary);
  if (ec || !in) {
    error = "can't open " + image.string();
    return false;
  }
  for (uint64_t partition : kPartitions) {
    uint8_t volume[kSector];
    if (partition + 33 * kSector > image_size ||
        !ReadAt(in, partition + 32 * kSector, volume, sizeof(volume)) ||
        std::memcmp(volume, kMagic, 20) != 0) {
      continue;
    }
    Lister lister{in, partition, image_size, files, {}};
    if (lister.Directory(Read32(volume + 20), Read32(volume + 24), "", 0) && !files.empty()) {
      // Each directory before its files, so it exists when they're written.
      std::stable_sort(files.begin(), files.end(), [](const DiscFile& a, const DiscFile& b) {
        return a.directory > b.directory;
      });
      return true;
    }
    files.clear();
    error = "damaged disc image";
    return false;
  }
  error = "not an Xbox 360 disc image";
  return false;
}

const DiscFile* FindRootFile(const std::vector<DiscFile>& files, const std::string& name) {
  for (const DiscFile& file : files) {
    if (!file.directory && SameName(file.path, name)) {
      return &file;
    }
  }
  return nullptr;
}

bool ExtractDiscImage(const std::filesystem::path& image, const std::vector<DiscFile>& files,
                      const std::filesystem::path& folder, const std::string& last,
                      ExtractProgress& progress,
                      const std::function<void(const std::string&)>& on_file,
                      std::string& error) {
  std::ifstream in(image, std::ios::binary);
  if (!in) {
    error = "can't open " + image.string();
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::vector<const DiscFile*> order;
  const DiscFile* last_file = FindRootFile(files, last);
  for (const DiscFile& file : files) {
    if (&file != last_file) {
      order.push_back(&file);
    }
  }
  if (last_file) {
    order.push_back(last_file);
    // An earlier, interrupted extraction's: it would say this one is whole.
    std::filesystem::remove(folder / last_file->path, ec);
  }
  std::vector<char> buffer(8u << 20);
  for (const DiscFile* file : order) {
    if (progress.cancel) {
      error = "cancelled";
      return false;
    }
    const std::filesystem::path target = folder / std::filesystem::path(file->path);
    if (file->directory) {
      std::filesystem::create_directories(target, ec);
      if (ec) {
        error = "can't create " + target.string() + ": " + ec.message();
        return false;
      }
      continue;
    }
    on_file(file->path);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "can't write " + target.string();
      return false;
    }
    in.clear();
    in.seekg(std::streamoff(file->offset));
    for (uint64_t left = file->size; left;) {
      if (progress.cancel) {
        out.close();
        std::filesystem::remove(target, ec);
        error = "cancelled";
        return false;
      }
      size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
      in.read(buffer.data(), std::streamsize(chunk));
      if (in.gcount() != std::streamsize(chunk)) {
        error = "can't read " + file->path + " from the image";
        return false;
      }
      out.write(buffer.data(), std::streamsize(chunk));
      if (!out) {
        error = "can't write " + target.string() + " (is the storage full?)";
        return false;
      }
      left -= chunk;
      progress.done += chunk;
    }
    out.close();
    if (!out) {
      error = "can't write " + target.string() + " (is the storage full?)";
      return false;
    }
  }
  return true;
}
