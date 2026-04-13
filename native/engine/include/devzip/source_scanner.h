#pragma once

#include "devzip/archive_format.h"

#include <filesystem>
#include <optional>
#include <string>

namespace devzip {

struct ScanOptions {
  bool include_hidden = true;
  bool fail_on_access_denied = true;
  bool follow_reparse_points = false;
  std::size_t maximum_depth = 128;
};

struct ScanResult {
  std::filesystem::path source_root;
  ArchiveManifest manifest;
  std::vector<std::filesystem::path> skipped_paths;
};

ScanResult scan_source_tree(const std::filesystem::path& source_root,
                            const ScanOptions& options = {});

}  // namespace devzip
