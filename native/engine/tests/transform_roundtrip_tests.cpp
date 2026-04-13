#include "devzip/transform_pipeline.h"
#include "test_harness.h"

#include <string>

namespace {

std::vector<std::byte> to_bytes(const std::string& text) {
  return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

}  // namespace

DEVZIP_TEST(code_dictionary_transform_round_trips_text) {
  const auto input = to_bytes("CompressionBackend CompressionBackend CompressionBackend token token token");

  std::string recipe;
  const auto encoded = devzip::transforms::apply_code_dictionary(input, recipe);
  const auto decoded = devzip::transforms::reverse_code_dictionary(encoded, recipe);

  DEVZIP_REQUIRE(!recipe.empty(), "Dictionary transform should produce a recipe for repeated text");
  DEVZIP_REQUIRE(decoded == input, "Dictionary transform should be reversible");
}

DEVZIP_TEST(stream_normalization_round_trips_gzip_header) {
  std::vector<std::byte> input = {
      std::byte{0x1F}, std::byte{0x8B}, std::byte{0x08}, std::byte{0x00},
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
      std::byte{0x02}, std::byte{0x03}, std::byte{0xAA}, std::byte{0xBB},
  };

  std::string recipe;
  const auto encoded = devzip::transforms::apply_stream_normalization(input, recipe);
  const auto decoded = devzip::transforms::reverse_stream_normalization(encoded, recipe);

  DEVZIP_REQUIRE(recipe.starts_with("gzip:"), "Gzip header normalization should emit a gzip recipe");
  DEVZIP_REQUIRE(decoded == input, "Stream normalization should be reversible");
}

DEVZIP_TEST(transform_pipeline_restores_original_file) {
  const auto input = to_bytes("alpha alpha alpha alpha\nbeta beta beta beta\n");
  devzip::TransformPipeline pipeline;
  const auto prepared = pipeline.prepare_file(input, "alpha.txt");

  devzip::ManifestEntry entry;
  entry.archive_path = "alpha.txt";
  entry.content_checksum = prepared.content_checksum;

  std::vector<devzip::DecodedBlock> decoded_blocks;
  for (const auto& block : prepared.blocks) {
    decoded_blocks.push_back({block.payload, block.transforms, block.recipe});
  }

  const auto restored = pipeline.restore_file(entry, decoded_blocks);
  DEVZIP_REQUIRE(restored == input, "Pipeline should restore the original file bytes");
}

DEVZIP_TEST(transform_pipeline_restores_normalized_gzip_like_input) {
  std::vector<std::byte> input = {
      std::byte{0x1F}, std::byte{0x8B}, std::byte{0x08}, std::byte{0x00},
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
      std::byte{0x02}, std::byte{0x03}, std::byte{0xAA}, std::byte{0xBB},
  };

  devzip::TransformPipeline pipeline;
  const auto prepared = pipeline.prepare_file(input, "small.gz");

  devzip::ManifestEntry entry;
  entry.archive_path = "small.gz";
  entry.content_checksum = prepared.content_checksum;

  std::vector<devzip::DecodedBlock> decoded_blocks;
  for (const auto& block : prepared.blocks) {
    decoded_blocks.push_back({block.payload, block.transforms, block.recipe});
  }

  const auto restored = pipeline.restore_file(entry, decoded_blocks);
  DEVZIP_REQUIRE(restored == input, "Pipeline should restore gzip-normalized bytes");
}

DEVZIP_TEST(transform_pipeline_detects_tampered_payloads) {
  const auto input = to_bytes("alpha alpha alpha alpha\n");
  devzip::TransformPipeline pipeline;
  const auto prepared = pipeline.prepare_file(input, "alpha.txt");

  devzip::ManifestEntry entry;
  entry.archive_path = "alpha.txt";
  entry.content_checksum = prepared.content_checksum;

  std::vector<devzip::DecodedBlock> decoded_blocks;
  for (const auto& block : prepared.blocks) {
    auto tampered = block.payload;
    if (!tampered.empty()) {
      tampered[0] = std::byte{0};
    }
    decoded_blocks.push_back({std::move(tampered), block.transforms, block.recipe});
  }

  bool threw = false;
  try {
    (void)pipeline.restore_file(entry, decoded_blocks);
  } catch (...) {
    threw = true;
  }

  DEVZIP_REQUIRE(threw, "Pipeline should reject tampered payloads");
}
