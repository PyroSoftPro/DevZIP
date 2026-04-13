#pragma once

#include "devzip/archive_format.h"
#include "devzip/backend.h"

#include <filesystem>

namespace devzip {

struct ArchiveWriteResult {
  std::filesystem::path archive_path;
  ArchiveManifest manifest;
};

ArchiveWriteResult create_archive(const std::filesystem::path& source_path,
                                  const std::filesystem::path& archive_path,
                                  CompressionBackend& backend);

ArchiveManifest read_archive_manifest(const std::filesystem::path& archive_path);

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination_root,
                     CompressionBackend& backend);
void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination_root);

void verify_archive(const std::filesystem::path& archive_path, CompressionBackend& backend);
void verify_archive(const std::filesystem::path& archive_path);

}  // namespace devzip
