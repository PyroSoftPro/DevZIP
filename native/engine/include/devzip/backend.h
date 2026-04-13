#pragma once

#include "devzip/archive_format.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devzip {

struct CompressionRequest {
  std::span<const std::byte> input;
  std::string content_name;
};

struct CompressionResponse {
  std::vector<std::byte> encoded;
  std::array<std::uint8_t, 16> checksum{};
  std::uint64_t raw_size = 0;
};

class CompressionBackend {
 public:
  virtual ~CompressionBackend() = default;

  virtual BackendStamp stamp() const = 0;
  virtual CompressionResponse compress(const CompressionRequest& request) = 0;
  virtual std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                            std::uint64_t expected_raw_size) const = 0;

  virtual void verify(std::span<const std::byte> encoded,
                      std::span<const std::byte> expected_plaintext) const;
};

std::unique_ptr<CompressionBackend> make_libzpaq_backend(std::string method = "5");
std::unique_ptr<CompressionBackend> make_lzma2_backend();
std::unique_ptr<CompressionBackend> make_ppmd_backend();
std::unique_ptr<CompressionBackend> make_best_of_two_backend(std::string zpaq_method = "4");
std::unique_ptr<CompressionBackend> make_best_of_three_backend();
std::unique_ptr<CompressionBackend> make_selective_zpaq_backend();
std::unique_ptr<CompressionBackend> make_backend(const BackendStamp& stamp);

}  // namespace devzip
