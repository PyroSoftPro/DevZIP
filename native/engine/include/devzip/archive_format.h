#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace devzip {

inline constexpr std::uint16_t kCurrentFormatVersion = 3;

enum class EntryKind : std::uint8_t {
  File = 1,
  Directory = 2,
};

enum class TransformKind : std::uint8_t {
  None = 0,
  ChunkReference = 1,
  StreamNormalization = 2,
  CodeDictionary = 3,
  BcjX86 = 4,
  DeltaFilter = 5,
  PngIdatStrip = 6,
};

enum class BlockStorageKind : std::uint8_t {
  BackendCompressed = 0,
  StoredRaw = 1,
};

struct BackendStamp {
  std::string name;
  std::string version;
};

struct BlockDescriptor {
  std::string id;
  std::uint64_t payload_offset = 0;
  std::uint64_t encoded_size = 0;
  std::uint64_t raw_size = 0;
  BlockStorageKind storage = BlockStorageKind::BackendCompressed;
  std::array<std::uint8_t, 16> checksum{};
  std::vector<TransformKind> transforms;
  std::string recipe;
};

struct BlockSpanReference {
  std::string block_id;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::array<std::uint8_t, 16> checksum{};
  std::vector<TransformKind> transforms;
  std::string recipe;
};

struct ManifestEntry {
  std::string archive_path;
  EntryKind kind = EntryKind::File;
  std::uint64_t size = 0;
  std::uint64_t modified_time_ns = 0;
  std::uint32_t windows_attributes = 0;
  std::array<std::uint8_t, 16> content_checksum{};
  std::vector<std::string> block_ids;
  std::vector<BlockSpanReference> block_spans;
};

struct ArchiveManifest {
  BackendStamp backend;
  std::string source_name;
  std::string created_utc;
  bool deterministic = true;
  std::vector<ManifestEntry> entries;
  std::vector<BlockDescriptor> blocks;
};

struct ArchiveHeader {
  std::array<char, 4> magic{'D', 'V', 'Z', '1'};
  std::uint16_t format_version = kCurrentFormatVersion;
  std::uint16_t header_size = 40;
  std::uint64_t manifest_size = 0;
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, 16> manifest_checksum{};
};

std::vector<std::byte> serialize_manifest(const ArchiveManifest& manifest,
                                          std::uint16_t format_version = kCurrentFormatVersion);
ArchiveManifest deserialize_manifest(std::span<const std::byte> bytes,
                                     std::uint16_t format_version = kCurrentFormatVersion);

std::filesystem::path default_archive_path(const std::filesystem::path& source_path,
                                           const std::filesystem::path& parent_directory);

std::string path_to_utf8(const std::filesystem::path& path);
std::string path_to_generic_utf8(const std::filesystem::path& path);

std::array<std::uint8_t, 16> checksum_bytes(std::span<const std::byte> bytes);
std::array<std::uint8_t, 16> fast_hash_bytes(std::span<const std::byte> bytes);

}  // namespace devzip
