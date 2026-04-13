#include "devzip/transform_pipeline.h"
#include "test_harness.h"

#include <algorithm>
#include <string>

namespace {

std::vector<std::byte> to_bytes(const std::string& text) {
  return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

}  // namespace

DEVZIP_TEST(code_dictionary_transform_reduces_repetitive_text) {
  const auto input =
      to_bytes("CompressionBackend CompressionBackend CompressionBackend CompressionBackend");
  std::string recipe;
  const auto encoded = devzip::transforms::apply_code_dictionary(input, recipe);

  DEVZIP_REQUIRE(!recipe.empty(), "Transform should detect a useful dictionary");
  DEVZIP_REQUIRE(encoded.size() < input.size(), "Encoded dictionary form should be smaller than input");
}

DEVZIP_TEST(fastcdc_chunker_splits_large_repeated_payloads) {
  std::string repeated;
  repeated.reserve(96 * 1024);
  for (int index = 0; index < 1024; ++index) {
    repeated += "alpha-beta-gamma-delta-";
  }

  const auto input = to_bytes(repeated + repeated + repeated);
  const auto chunks = devzip::transforms::fastcdc_chunk_bytes(input);

  DEVZIP_REQUIRE(chunks.size() > 1, "Large repeated payloads should split into multiple chunks");
}

DEVZIP_TEST(transform_pipeline_marks_chunked_blocks) {
  std::string repeated;
  repeated.reserve(128 * 1024);
  for (int index = 0; index < 8192; ++index) {
    repeated += "line-with-shared-prefix-and-body\n";
  }

  devzip::TransformPipeline pipeline;
  const auto prepared = pipeline.prepare_file(to_bytes(repeated), "logs.txt");

  DEVZIP_REQUIRE(prepared.blocks.size() > 1, "Large repeated text should emit multiple prepared blocks");
  DEVZIP_REQUIRE(std::find(prepared.blocks.front().transforms.begin(),
                           prepared.blocks.front().transforms.end(),
                           devzip::TransformKind::ChunkReference) != prepared.blocks.front().transforms.end(),
                 "Chunked files should record the chunk-reference transform");
}

DEVZIP_TEST(transform_pipeline_keeps_large_binary_files_unchunked) {
  std::vector<std::byte> binary(256 * 1024);
  for (std::size_t index = 0; index < binary.size(); ++index) {
    binary[index] = static_cast<std::byte>(index & 0xFF);
  }

  devzip::TransformPipeline pipeline;
  const auto prepared = pipeline.prepare_file(binary, "images.bin");

  DEVZIP_REQUIRE(prepared.blocks.size() == 1,
                 "Large binary payloads should stay single-block when solid grouping already handles them");
  DEVZIP_REQUIRE(std::find(prepared.blocks.front().transforms.begin(),
                           prepared.blocks.front().transforms.end(),
                           devzip::TransformKind::ChunkReference) == prepared.blocks.front().transforms.end(),
                 "Unchunked binary payloads should not record chunk-reference transforms");
}
