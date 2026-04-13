#pragma once

#include "devzip/archive_format.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devzip {

struct PreparedBlock {
  std::vector<std::byte> payload;
  std::vector<std::byte> fallback_payload;
  std::array<std::uint8_t, 16> payload_checksum{};
  std::array<std::uint8_t, 16> fallback_checksum{};
  std::vector<TransformKind> transforms;
  std::string recipe;
  bool prefer_stored_raw = false;
};

struct PreparedFile {
  std::vector<PreparedBlock> blocks;
  std::array<std::uint8_t, 16> content_checksum{};
};

struct DecodedBlock {
  std::vector<std::byte> payload;
  std::vector<TransformKind> transforms;
  std::string recipe;
};

namespace transforms {

std::vector<std::vector<std::byte>> fastcdc_chunk_bytes(std::span<const std::byte> input);

bool looks_like_text(std::span<const std::byte> input);

std::vector<std::byte> apply_stream_normalization(std::span<const std::byte> input, std::string& recipe);
std::vector<std::byte> reverse_stream_normalization(std::span<const std::byte> input, std::string_view recipe);

std::vector<std::byte> apply_code_dictionary(std::span<const std::byte> input, std::string& recipe);
std::vector<std::byte> reverse_code_dictionary(std::span<const std::byte> input, std::string_view recipe);

std::vector<std::byte> apply_bcj_x86(std::span<const std::byte> input);
std::vector<std::byte> reverse_bcj_x86(std::span<const std::byte> input);

std::vector<std::byte> apply_delta_filter(std::span<const std::byte> input);
std::vector<std::byte> reverse_delta_filter(std::span<const std::byte> input);

std::vector<std::byte> apply_png_idat_strip(std::span<const std::byte> input,
                                            std::string& recipe_out);
std::vector<std::byte> reverse_png_idat_strip(std::span<const std::byte> raw_pixels,
                                              const std::string& recipe_str);

}  // namespace transforms

class TransformPipeline {
 public:
  PreparedFile prepare_file(std::span<const std::byte> input, std::string_view archive_path) const;
  std::vector<std::byte> restore_file(const ManifestEntry& entry,
                                      const std::vector<DecodedBlock>& blocks) const;
};

}  // namespace devzip
