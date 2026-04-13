#include "devzip/backend.h"

#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace devzip {
namespace {

// Tag bytes embedded as the first byte of every compressed block so the
// decompressor knows which inner codec produced it.
constexpr std::byte kTagLzma2{0x4C};  // 'L'
constexpr std::byte kTagZpaq{0x5A};   // 'Z'

class BestOfTwoBackend final : public CompressionBackend {
 public:
  explicit BestOfTwoBackend(std::string zpaq_method)
      : lzma2_(make_lzma2_backend()),
        zpaq_(make_libzpaq_backend(zpaq_method)),
        zpaq_method_(std::move(zpaq_method)) {}

  BackendStamp stamp() const override { return {"best-of-two", "lzma2+zpaq-m" + zpaq_method_}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    // Run both compressors concurrently: LZMA2 on a worker thread, ZPAQ on
    // the current thread.  Both see the same read-only request.input span and
    // operate on fully independent state, so there is no data race.
    auto lzma2_future = std::async(std::launch::async,
                                   [&]() { return lzma2_->compress(request); });
    auto zpaq_response  = zpaq_->compress(request);
    auto lzma2_response = lzma2_future.get();

    const bool lzma2_wins = lzma2_response.encoded.size() <= zpaq_response.encoded.size();
    auto& winner          = lzma2_wins ? lzma2_response : zpaq_response;
    const auto tag        = lzma2_wins ? kTagLzma2 : kTagZpaq;

    // Build tagged output: [1-byte tag][winner encoded data]
    std::vector<std::byte> tagged;
    tagged.reserve(1 + winner.encoded.size());
    tagged.push_back(tag);
    tagged.insert(tagged.end(), winner.encoded.begin(), winner.encoded.end());
    winner.encoded = std::move(tagged);
    return winner;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) {
      throw std::runtime_error("best-of-two: empty payload");
    }
    const auto tag     = encoded.front();
    const auto payload = encoded.subspan(1);

    if (tag == kTagLzma2) {
      return lzma2_->decompress(payload, expected_raw_size);
    }
    if (tag == kTagZpaq) {
      return zpaq_->decompress(payload, expected_raw_size);
    }
    throw std::runtime_error("best-of-two: unknown codec tag byte");
  }

 private:
  std::unique_ptr<CompressionBackend> lzma2_;
  std::unique_ptr<CompressionBackend> zpaq_;
  std::string zpaq_method_;
};

}  // namespace

std::unique_ptr<CompressionBackend> make_best_of_two_backend(std::string zpaq_method) {
  return std::make_unique<BestOfTwoBackend>(std::move(zpaq_method));
}

}  // namespace devzip
