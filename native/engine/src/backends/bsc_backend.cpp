#include "devzip/backend.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#if defined(DEVZIP_HAVE_BSC)
#include <libbsc/libbsc.h>
#endif

namespace devzip {
namespace {

#if defined(DEVZIP_HAVE_BSC)

// libbsc requires a one-time global initialisation before any compress/decode.
void ensure_bsc_initialised() {
  static std::once_flag flag;
  static int init_result = LIBBSC_NO_ERROR;
  std::call_once(flag, [] { init_result = bsc_init(LIBBSC_FEATURE_NONE); });
  if (init_result != LIBBSC_NO_ERROR) {
    throw std::runtime_error("libbsc initialisation failed");
  }
}

// We prepend a single framing byte so an empty / stored block round-trips
// without invoking libbsc on a zero-length buffer (which it rejects).
constexpr std::byte kBscStored{0x00};
constexpr std::byte kBscCompressed{0x01};

class BscBackend final : public CompressionBackend {
 public:
  BscBackend() { ensure_bsc_initialised(); }

  BackendStamp stamp() const override { return {"bsc", LIBBSC_VERSION_STRING}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    if (request.input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("libbsc backend cannot encode payloads larger than INT_MAX bytes");
    }

    CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = checksum_bytes(request.input);

    const int n = static_cast<int>(request.input.size());
    if (n == 0) {
      response.encoded.push_back(kBscStored);
      return response;
    }

    std::vector<unsigned char> output(static_cast<std::size_t>(n) + LIBBSC_HEADER_SIZE);
    const int result = bsc_compress(
        reinterpret_cast<const unsigned char*>(request.input.data()), output.data(), n,
        LIBBSC_DEFAULT_LZPHASHSIZE, LIBBSC_DEFAULT_LZPMINLEN, LIBBSC_BLOCKSORTER_BWT,
        LIBBSC_CODER_QLFC_STATIC, LIBBSC_FEATURE_NONE);

    if (result == LIBBSC_NOT_COMPRESSIBLE || result < 0) {
      // Fall back to a verbatim copy framed as "stored".
      response.encoded.reserve(static_cast<std::size_t>(n) + 1);
      response.encoded.push_back(kBscStored);
      response.encoded.insert(response.encoded.end(), request.input.begin(), request.input.end());
      return response;
    }

    response.encoded.reserve(static_cast<std::size_t>(result) + 1);
    response.encoded.push_back(kBscCompressed);
    const auto* begin = reinterpret_cast<const std::byte*>(output.data());
    response.encoded.insert(response.encoded.end(), begin, begin + result);
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) {
      throw std::runtime_error("libbsc backend: empty payload");
    }
    const auto frame = encoded.front();
    const auto body = encoded.subspan(1);

    if (frame == kBscStored) {
      return std::vector<std::byte>(body.begin(), body.end());
    }
    if (frame != kBscCompressed) {
      throw std::runtime_error("libbsc backend: unknown frame byte");
    }
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("libbsc backend cannot decode payloads larger than INT_MAX bytes");
    }

    int block_size = 0;
    int data_size = 0;
    const int info = bsc_block_info(reinterpret_cast<const unsigned char*>(body.data()),
                                    LIBBSC_HEADER_SIZE, &block_size, &data_size,
                                    LIBBSC_FEATURE_NONE);
    if (info != LIBBSC_NO_ERROR) {
      throw std::runtime_error("libbsc backend: corrupt block header");
    }
    if (static_cast<std::size_t>(block_size) != body.size()) {
      throw std::runtime_error("libbsc backend: block size mismatch");
    }

    std::vector<std::byte> output(static_cast<std::size_t>(data_size));
    const int result = bsc_decompress(
        reinterpret_cast<const unsigned char*>(body.data()), block_size,
        reinterpret_cast<unsigned char*>(output.data()), data_size, LIBBSC_FEATURE_NONE);
    if (result != LIBBSC_NO_ERROR) {
      throw std::runtime_error("libbsc backend: decompression failed");
    }
    if (expected_raw_size != 0 && output.size() != expected_raw_size) {
      throw std::runtime_error("libbsc backend: decoded size mismatch");
    }
    return output;
  }
};

#endif  // DEVZIP_HAVE_BSC

}  // namespace

#if defined(DEVZIP_HAVE_BSC)
bool bsc_backend_available() { return true; }
std::unique_ptr<CompressionBackend> make_bsc_backend() {
  return std::make_unique<BscBackend>();
}
#else
bool bsc_backend_available() { return false; }
std::unique_ptr<CompressionBackend> make_bsc_backend() { return nullptr; }
#endif

}  // namespace devzip
