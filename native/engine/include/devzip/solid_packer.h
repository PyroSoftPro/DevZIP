#pragma once

#include "devzip/transform_pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace devzip {

struct SolidPackItem {
  std::size_t entry_index = 0;
  std::size_t block_index = 0;
  std::string archive_path;
  PreparedBlock block;
  bool prefer_stored_raw = false;
};

struct SolidPackSpan {
  std::size_t entry_index = 0;
  std::size_t block_index = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, 16> payload_checksum{};
  std::vector<TransformKind> payload_transforms;
  std::string payload_recipe;
  std::uint64_t fallback_offset = 0;
  std::uint64_t fallback_size = 0;
  std::array<std::uint8_t, 16> fallback_checksum{};
  std::vector<TransformKind> fallback_transforms;
};

struct SolidPackGroup {
  std::vector<std::byte> payload;
  std::vector<std::byte> fallback_payload;
  std::vector<SolidPackSpan> spans;
  bool prefer_stored_raw = false;
};

class SolidPacker {
 public:
  std::vector<SolidPackGroup> pack(const std::vector<SolidPackItem>& items) const;
};

}  // namespace devzip
