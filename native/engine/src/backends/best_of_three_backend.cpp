#include "devzip/backend.h"

#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

namespace devzip {
namespace {

constexpr std::byte kTagLzma2{0x4C};  // 'L'
constexpr std::byte kTagZpaq{0x5A};   // 'Z'
constexpr std::byte kTagPpmd{0x50};   // 'P'

class BestOfThreeBackend final : public CompressionBackend {
 public:
  BestOfThreeBackend()
      : lzma2_(make_lzma2_backend()),
        zpaq_(make_libzpaq_backend("4")),
        ppmd_(make_ppmd_backend()) {}

  BackendStamp stamp() const override { return {"best-of-three", "lzma2+zpaq-m4+ppmd"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    auto lzma_future = std::async(std::launch::async, [&]() {
      return lzma2_->compress(request);
    });
    auto ppmd_future = std::async(std::launch::async, [&]() {
      return ppmd_->compress(request);
    });
    auto zpaq_response = zpaq_->compress(request);
    auto lzma_response = lzma_future.get();
    auto ppmd_response = ppmd_future.get();

    CompressionResponse* winner = &lzma_response;
    std::byte tag = kTagLzma2;
    if (zpaq_response.encoded.size() < winner->encoded.size()) {
      winner = &zpaq_response;
      tag = kTagZpaq;
    }
    if (ppmd_response.encoded.size() < winner->encoded.size()) {
      winner = &ppmd_response;
      tag = kTagPpmd;
    }

    std::vector<std::byte> tagged;
    tagged.reserve(1 + winner->encoded.size());
    tagged.push_back(tag);
    tagged.insert(tagged.end(), winner->encoded.begin(), winner->encoded.end());
    winner->encoded = std::move(tagged);
    return std::move(*winner);
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) {
      throw std::runtime_error("best-of-three: empty payload");
    }

    const auto tag = encoded.front();
    const auto payload = encoded.subspan(1);
    if (tag == kTagLzma2) {
      return lzma2_->decompress(payload, expected_raw_size);
    }
    if (tag == kTagZpaq) {
      return zpaq_->decompress(payload, expected_raw_size);
    }
    if (tag == kTagPpmd) {
      return ppmd_->decompress(payload, expected_raw_size);
    }
    throw std::runtime_error("best-of-three: unknown codec tag byte");
  }

 private:
  std::unique_ptr<CompressionBackend> lzma2_;
  std::unique_ptr<CompressionBackend> zpaq_;
  std::unique_ptr<CompressionBackend> ppmd_;
};

}  // namespace

std::unique_ptr<CompressionBackend> make_best_of_three_backend() {
  return std::make_unique<BestOfThreeBackend>();
}

}  // namespace devzip
