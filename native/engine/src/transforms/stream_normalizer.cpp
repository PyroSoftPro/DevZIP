#include "devzip/transform_pipeline.h"

#include <stdexcept>
#include <string>

namespace devzip::transforms {
namespace {

std::string bytes_to_hex(std::span<const std::byte> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = static_cast<std::uint8_t>(byte);
    output.push_back(kHex[(value >> 4) & 0x0F]);
    output.push_back(kHex[value & 0x0F]);
  }
  return output;
}

std::vector<std::byte> hex_to_bytes(std::string_view hex) {
  auto decode = [](char ch) -> std::uint8_t {
    if (ch >= '0' && ch <= '9') {
      return static_cast<std::uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
      return static_cast<std::uint8_t>(10 + (ch - 'a'));
    }
    if (ch >= 'A' && ch <= 'F') {
      return static_cast<std::uint8_t>(10 + (ch - 'A'));
    }
    throw std::runtime_error("Invalid hex digit in stream normalization recipe");
  };

  std::vector<std::byte> output;
  output.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    const auto high = decode(hex[index]);
    const auto low = decode(hex[index + 1]);
    output.push_back(static_cast<std::byte>((high << 4) | low));
  }
  return output;
}

}  // namespace

std::vector<std::byte> apply_stream_normalization(std::span<const std::byte> input, std::string& recipe) {
  recipe.clear();
  std::vector<std::byte> output(input.begin(), input.end());

  if (input.size() >= 10 &&
      static_cast<std::uint8_t>(input[0]) == 0x1F &&
      static_cast<std::uint8_t>(input[1]) == 0x8B &&
      static_cast<std::uint8_t>(input[2]) == 0x08 &&
      static_cast<std::uint8_t>(input[3]) == 0x00) {
    recipe = "gzip:" + bytes_to_hex(input.subspan(4, 6));
    for (std::size_t index = 4; index < 10; ++index) {
      output[index] = std::byte{0};
    }
  }

  return output;
}

std::vector<std::byte> reverse_stream_normalization(std::span<const std::byte> input, std::string_view recipe) {
  if (!recipe.starts_with("gzip:")) {
    return std::vector<std::byte>(input.begin(), input.end());
  }

  std::vector<std::byte> output(input.begin(), input.end());
  const auto original = hex_to_bytes(recipe.substr(5));
  if (output.size() >= 10 && original.size() == 6) {
    for (std::size_t index = 0; index < original.size(); ++index) {
      output[4 + index] = original[index];
    }
  }
  return output;
}

}  // namespace devzip::transforms
