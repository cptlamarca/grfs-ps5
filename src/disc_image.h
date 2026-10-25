// The game's Xbox 360 disc image (an .iso of its XDVDFS file system): its
// files listed and extracted, for players who have the image but not its
// files.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct DiscFile {
  std::string path;  // relative, '/' separated, as on the disc
  uint64_t offset = 0;
  uint64_t size = 0;
  bool directory = false;
};

// Lists the files of an image (directories before their files). False with
// an error message if it isn't an Xbox disc image.
bool ListDiscImage(const std::filesystem::path& image, std::vector<DiscFile>& files,
                   std::string& error);

// The file of `files` at the disc's root named `name`, any case; null if none.
const DiscFile* FindRootFile(const std::vector<DiscFile>& files, const std::string& name);

struct ExtractProgress {
  std::atomic<uint64_t> done{0};
  std::atomic<bool> cancel{false};
};

// Extracts `files` of the image into `folder`; `on_file` gets each file's
// path as it starts. `last` (a root file name, any case) is written last, so
// that its presence means a whole extraction. False with an error message
// if it fails or is cancelled.
bool ExtractDiscImage(const std::filesystem::path& image, const std::vector<DiscFile>& files,
                      const std::filesystem::path& folder, const std::string& last,
                      ExtractProgress& progress,
                      const std::function<void(const std::string&)>& on_file,
                      std::string& error);
