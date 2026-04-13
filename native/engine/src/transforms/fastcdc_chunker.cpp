#include "devzip/transform_pipeline.h"

#include <algorithm>

namespace devzip::transforms {

std::vector<std::vector<std::byte>> fastcdc_chunk_bytes(std::span<const std::byte> input) {
  constexpr std::size_t kMinimumChunk = 8 * 1024;
  constexpr std::size_t kAverageChunk = 16 * 1024;
  constexpr std::size_t kMaximumChunk = 64 * 1024;
  constexpr std::uint32_t kMask = static_cast<std::uint32_t>(kAverageChunk - 1);

  if (input.size() <= kMaximumChunk) {
    return {std::vector<std::byte>(input.begin(), input.end())};
  }

  std::vector<std::vector<std::byte>> chunks;
  std::size_t start = 0;
  std::uint32_t rolling_hash = 0;

  for (std::size_t index = 0; index < input.size(); ++index) {
    rolling_hash = (rolling_hash * 33u) ^ static_cast<std::uint8_t>(input[index]);
    const auto chunk_size = index - start + 1;

    if (chunk_size < kMinimumChunk) {
      continue;
    }

    if (chunk_size >= kMaximumChunk || (rolling_hash & kMask) == 0) {
      chunks.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start),
                          input.begin() + static_cast<std::ptrdiff_t>(index + 1));
      start = index + 1;
      rolling_hash = 0;
    }
  }

  if (start < input.size()) {
    chunks.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start), input.end());
  }

  return chunks;
}

bool looks_like_text(std::span<const std::byte> input) {
  if (input.empty()) {
    return false;
  }

  std::size_t printable = 0;
  for (const auto byte : input) {
    const auto value = static_cast<unsigned char>(byte);
    if ((value >= 32 && value <= 126) || value == '\n' || value == '\r' || value == '\t') {
      ++printable;
    }
  }
  return printable * 100 / input.size() >= 85;
}

}  // namespace devzip::transforms
