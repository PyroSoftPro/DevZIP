#include "devzip/archive_format.h"
#include "devzip/source_scanner.h"
#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
  auto root = std::filesystem::temp_directory_path() / ("devzip-" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
}

}  // namespace

DEVZIP_TEST(manifest_serialization_round_trips_metadata) {
  devzip::ArchiveManifest manifest;
  manifest.backend = {"libzpaq", "7.15"};
  manifest.source_name = "fixtures";
  manifest.created_utc = "2026-04-09T00:00:00Z";
  manifest.deterministic = true;

  devzip::ManifestEntry entry;
  entry.archive_path = "src/main.cpp";
  entry.kind = devzip::EntryKind::File;
  entry.size = 42;
  entry.modified_time_ns = 123456;
  entry.windows_attributes = 0x20;
  entry.content_checksum[0] = 7;
  entry.block_ids = {"block-1", "block-2"};
  entry.block_spans.push_back(devzip::BlockSpanReference{
      "block-1",
      16,
      32,
      {9},
      {devzip::TransformKind::CodeDictionary},
      "dictionary\n"});
  manifest.entries.push_back(entry);

  devzip::BlockDescriptor block;
  block.id = "block-1";
  block.payload_offset = 128;
  block.encoded_size = 64;
  block.raw_size = 96;
  block.storage = devzip::BlockStorageKind::StoredRaw;
  block.transforms = {devzip::TransformKind::ChunkReference, devzip::TransformKind::CodeDictionary};
  block.recipe = "dictionary\n";
  manifest.blocks.push_back(block);

  const auto encoded = devzip::serialize_manifest(manifest, devzip::kCurrentFormatVersion);
  const auto decoded = devzip::deserialize_manifest(encoded, devzip::kCurrentFormatVersion);

  DEVZIP_REQUIRE(decoded.backend.name == "libzpaq", "Backend name should round-trip");
  DEVZIP_REQUIRE(decoded.entries.size() == 1, "Expected one manifest entry");
  DEVZIP_REQUIRE(decoded.entries.front().archive_path == "src/main.cpp", "Archive path should round-trip");
  DEVZIP_REQUIRE(decoded.entries.front().content_checksum[0] == 7, "File checksum should round-trip");
  DEVZIP_REQUIRE(decoded.blocks.size() == 1, "Expected one block");
  DEVZIP_REQUIRE(decoded.blocks.front().storage == devzip::BlockStorageKind::StoredRaw,
                 "Block storage kind should round-trip");
  DEVZIP_REQUIRE(decoded.entries.front().block_spans.size() == 1, "Block span references should round-trip");
  DEVZIP_REQUIRE(decoded.entries.front().block_spans.front().offset == 16,
                 "Block span offsets should round-trip");
  DEVZIP_REQUIRE(decoded.entries.front().block_spans.front().transforms.size() == 1,
                 "Block span transforms should round-trip");
  DEVZIP_REQUIRE(decoded.blocks.front().transforms.size() == 2, "Transform list should round-trip");
  DEVZIP_REQUIRE(decoded.blocks.front().recipe == "dictionary\n", "Block recipe should round-trip");
}

DEVZIP_TEST(source_scanner_produces_deterministic_paths) {
  const auto root = make_temp_root("scanner");
  write_text(root / "b.txt", "b");
  write_text(root / "a" / "alpha.txt", "alpha");
  std::filesystem::create_directories(root / "empty");

  const auto scan = devzip::scan_source_tree(root);

  DEVZIP_REQUIRE(!scan.manifest.entries.empty(), "Scanner should emit entries");
  DEVZIP_REQUIRE(scan.manifest.entries.front().archive_path == ".", "Root directory should be present");
  DEVZIP_REQUIRE(scan.manifest.entries[1].archive_path == "a", "Entries should be sorted deterministically");
}

DEVZIP_TEST(default_archive_path_avoids_collisions) {
  const auto root = make_temp_root("output-path");
  write_text(root / "photos.dvz", "taken");

  const auto candidate = devzip::default_archive_path(root / "photos", root);
  DEVZIP_REQUIRE(devzip::path_to_utf8(candidate.filename()) == "photos (2).dvz",
                 "Archive name should avoid collisions");
}

DEVZIP_TEST(legacy_manifest_v1_deserializes_without_block_spans) {
  devzip::ArchiveManifest manifest;
  manifest.backend = {"libzpaq", "7.15"};
  manifest.source_name = "fixtures";
  manifest.created_utc = "2026-04-09T00:00:00Z";
  manifest.deterministic = true;

  devzip::ManifestEntry entry;
  entry.archive_path = "src/main.cpp";
  entry.kind = devzip::EntryKind::File;
  entry.block_ids = {"block-1"};
  manifest.entries.push_back(entry);

  devzip::BlockDescriptor block;
  block.id = "block-1";
  block.raw_size = 96;
  block.encoded_size = 64;
  manifest.blocks.push_back(block);

  const auto encoded = devzip::serialize_manifest(manifest, 1);
  const auto decoded = devzip::deserialize_manifest(encoded, 1);

  DEVZIP_REQUIRE(decoded.entries.size() == 1, "Legacy manifest should preserve entries");
  DEVZIP_REQUIRE(decoded.entries.front().block_ids.size() == 1, "Legacy manifest should preserve block ids");
  DEVZIP_REQUIRE(decoded.entries.front().block_spans.empty(),
                 "Legacy manifest should decode without synthetic block spans");
}
