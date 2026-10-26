// See disc_image.h. XDVDFS: 2048 byte sectors from the game partition's
// start (where it is depends on the disc's format), its volume descriptor at
// sector 32 ("MICROSOFT*XBOX*MEDIA", then the root directory's sector and
// size). A directory is a binary tree of entries, each: the left and right
// entries' offsets in 4 byte units (u16, 0 none), the data's sector and size
// (u32), attributes (u8, 0x10 a directory), the name's length (u8) and the
// name; entries are 4 byte aligned and don't cross sectors. Little endian.

#include "disc_image.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

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

namespace {

// A file descriptor, closed on scope exit.
struct Fd {
  int fd = -1;
  explicit Fd(int value) : fd(value) {}
  ~Fd() {
    if (fd >= 0) {
      close(fd);
    }
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

bool WriteAll(int fd, const char* data, size_t size) {
  while (size) {
    ssize_t written = write(fd, data, size);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    data += written;
    size -= size_t(written);
  }
  return true;
}

bool ReadAll(int fd, char* data, size_t size, uint64_t offset) {
  while (size) {
    ssize_t read = pread(fd, data, size, off_t(offset));
    if (read <= 0) {
      if (read < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    data += read;
    size -= size_t(read);
    offset += uint64_t(read);
  }
  return true;
}

// Drops a file's pages from the cache (size 0: to its end), where the
// system has posix_fadvise (the PS5's C library doesn't).
void DropFromCache(int fd, uint64_t offset, uint64_t size) {
#if defined(__PROSPERO__)
  (void)fd, (void)offset, (void)size;
#else
  posix_fadvise(fd, off_t(offset), off_t(size), POSIX_FADV_DONTNEED);
#endif
}

}  // namespace

// Plain system calls (the PS5's C++ streams move a few MB a second), the
// image read ahead on a thread while the files are written.
bool ExtractDiscImage(const std::filesystem::path& image, const std::vector<DiscFile>& files,
                      const std::filesystem::path& folder, const std::string& last,
                      ExtractProgress& progress,
                      const std::function<void(const std::string&)>& on_file,
                      std::string& error) {
  Fd in(open(image.c_str(), O_RDONLY));
  if (in.fd < 0) {
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

  // The chunks to copy, in order: each file's data in pieces of kChunk.
  constexpr size_t kChunk = 8u << 20, kBuffers = 4;
  constexpr uint64_t kFlushEvery = 64u << 20;
  struct Chunk {
    uint64_t offset;
    size_t size;
  };
  struct Slot {
    std::vector<char> data = std::vector<char>(kChunk);
    size_t size = 0;
    bool ok = false;
  };
  std::vector<Slot> slots(kBuffers);
  std::mutex mutex;
  std::condition_variable changed;
  uint64_t produced = 0, consumed = 0;  // chunks read, written
  bool stop = false;
  std::vector<Chunk> chunks;
  for (const DiscFile* file : order) {
    for (uint64_t at = 0; !file->directory && at < file->size; at += kChunk) {
      chunks.push_back({file->offset + at, size_t(std::min<uint64_t>(kChunk, file->size - at))});
    }
  }
  std::thread reader([&] {
    for (uint64_t i = 0; i < chunks.size(); ++i) {
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stop || i - consumed < kBuffers; });
        if (stop) {
          return;
        }
      }
      Slot& slot = slots[i % kBuffers];
      slot.size = chunks[i].size;
      const auto start = std::chrono::steady_clock::now();
      slot.ok = ReadAll(in.fd, slot.data.data(), chunks[i].size, chunks[i].offset);
      // (read once: out of the cache, which the files' writes need)
      DropFromCache(in.fd, chunks[i].offset, chunks[i].size);
      progress.read_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      {
        std::lock_guard<std::mutex> lock(mutex);
        produced = i + 1;
      }
      changed.notify_all();
    }
  });
  auto finish = [&](bool ok) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    changed.notify_all();
    reader.join();
    return ok;
  };

  uint64_t next = 0;
  for (const DiscFile* file : order) {
    if (progress.cancel) {
      error = "cancelled";
      return finish(false);
    }
    const std::filesystem::path target = folder / std::filesystem::path(file->path);
    if (file->directory) {
      std::filesystem::create_directories(target, ec);
      if (ec) {
        error = "can't create " + target.string() + ": " + ec.message();
        return finish(false);
      }
      continue;
    }
    on_file(file->path);
    Fd out(open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
    uint64_t unflushed = 0;
    if (out.fd < 0) {
      error = "can't write " + target.string() + ": " + std::strerror(errno);
      return finish(false);
    }
    for (uint64_t left = file->size; left;) {
      if (progress.cancel) {
        close(out.fd);
        out.fd = -1;
        std::filesystem::remove(target, ec);
        error = "cancelled";
        return finish(false);
      }
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return produced > next; });
      }
      Slot& slot = slots[next % kBuffers];
      if (!slot.ok) {
        error = "can't read " + file->path + " from the image";
        return finish(false);
      }
      const auto start = std::chrono::steady_clock::now();
      const bool written = WriteAll(out.fd, slot.data.data(), slot.size);
      progress.write_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
      if (!written) {
        error = "can't write " + target.string() + ": " + std::strerror(errno);
        return finish(false);
      }
      left -= slot.size;
      progress.done += slot.size;
      // Written out every 64 MB: on the PS5, a full cache of waiting
      // writes slows them to a few MB a second.
      unflushed += slot.size;
      if (unflushed >= kFlushEvery || !left) {
        fsync(out.fd);
        DropFromCache(out.fd, 0, 0);
        unflushed = 0;
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        consumed = ++next;
      }
      changed.notify_all();
    }
    if (close(out.fd) != 0) {
      out.fd = -1;
      error = "can't write " + target.string() + ": " + std::strerror(errno);
      return finish(false);
    }
    out.fd = -1;
  }
  return finish(true);
}
