#include "devzip/backend.h"

#include "libzpaq.h"

#include <limits>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

namespace libzpaq {

void error(const char* message) {
  if (message != nullptr && std::strstr(message, "ut of memory") != nullptr) {
    throw std::bad_alloc();
  }
  throw std::runtime_error(message == nullptr ? "libzpaq error" : message);
}

}  // namespace libzpaq

namespace devzip {
namespace {

class LibZpaqBackend final : public CompressionBackend {
 public:
  explicit LibZpaqBackend(std::string method) : method_(std::move(method)) {}

  BackendStamp stamp() const override { return {"libzpaq", "7.15"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    if (request.input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("libzpaq backend cannot encode payloads larger than INT_MAX bytes");
    }

    libzpaq::StringBuffer input;
    libzpaq::StringBuffer output;

    if (!request.input.empty()) {
      input.write(reinterpret_cast<const char*>(request.input.data()),
                  static_cast<int>(request.input.size()));
    }

    libzpaq::compress(&input, &output, method_.c_str(),
                      request.content_name.empty() ? nullptr : request.content_name.c_str(),
                      "dvz-backend", true);

    CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = checksum_bytes(request.input);
    response.encoded.resize(output.size());
    if (output.size() > 0) {
      std::memcpy(response.encoded.data(), output.data(), output.size());
    }
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t /*expected_raw_size*/) const override {
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("libzpaq backend cannot decode payloads larger than INT_MAX bytes");
    }

    libzpaq::StringBuffer input;
    libzpaq::StringBuffer output;

    if (!encoded.empty()) {
      input.write(reinterpret_cast<const char*>(encoded.data()), static_cast<int>(encoded.size()));
    }

    libzpaq::decompress(&input, &output);

    std::vector<std::byte> decoded(output.size());
    if (output.size() > 0) {
      std::memcpy(decoded.data(), output.data(), output.size());
    }
    return decoded;
  }

 private:
  std::string method_;
};

}  // namespace

void CompressionBackend::verify(std::span<const std::byte> encoded,
                                std::span<const std::byte> expected_plaintext) const {
  const auto decoded = decompress(encoded, static_cast<std::uint64_t>(expected_plaintext.size()));
  if (decoded.size() != expected_plaintext.size()) {
    throw std::runtime_error("Backend verification failed: size mismatch");
  }
  if (!decoded.empty() &&
      std::memcmp(decoded.data(), expected_plaintext.data(), decoded.size()) != 0) {
    throw std::runtime_error("Backend verification failed: payload mismatch");
  }
}

std::unique_ptr<CompressionBackend> make_libzpaq_backend(std::string method) {
  return std::make_unique<LibZpaqBackend>(std::move(method));
}

}  // namespace devzip
