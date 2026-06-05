#include "devzip/backend.h"

#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace devzip {
namespace {

// Per-block codec tags (first byte of each best-of-N block).
constexpr std::byte kTagLzma2{0x4C};  // 'L'
constexpr std::byte kTagZpaq{0x5A};   // 'Z'
constexpr std::byte kTagPpmd{0x50};   // 'P'
constexpr std::byte kTagBsc{0x42};    // 'B'

std::vector<std::string> split_spec(const std::string& spec) {
  std::vector<std::string> parts;
  std::stringstream ss(spec);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      parts.push_back(item);
    }
  }
  return parts;
}

// best-of-N: attempt every configured codec on the same input and keep the
// smallest result.  Decoding always has every codec available and dispatches on
// the per-block tag byte, so the configured set only affects compression.
class BestOfNBackend final : public CompressionBackend {
 public:
  explicit BestOfNBackend(std::string spec) : spec_(std::move(spec)) {
    bool any = false;
    for (const auto& token : split_spec(spec_)) {
      if (token == "lzma2") {
        use_lzma2_ = true;
        any = true;
      } else if (token.rfind("zpaq", 0) == 0) {
        zpaq_method_ = token.size() > 4 ? token.substr(4) : "5";
        use_zpaq_ = true;
        any = true;
      } else if (token == "ppmd") {
        use_ppmd_ = true;
        any = true;
      } else if (token == "bsc" && bsc_backend_available()) {
        use_bsc_ = true;
        any = true;
      }
    }
    if (!any) {
      use_lzma2_ = true;  // never produce an empty trial set
    }
  }

  BackendStamp stamp() const override { return {"best-of-n", spec_}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    // Launch each configured codec concurrently; they share the read-only input
    // span and keep independent state.
    std::future<CompressionResponse> lzma2_f;
    std::future<CompressionResponse> zpaq_f;
    std::future<CompressionResponse> ppmd_f;
    std::future<CompressionResponse> bsc_f;

    if (use_lzma2_) {
      lzma2_f = std::async(std::launch::async, [&] { return make_lzma2_backend()->compress(request); });
    }
    if (use_zpaq_) {
      zpaq_f = std::async(std::launch::async,
                          [&] { return make_libzpaq_backend(zpaq_method_)->compress(request); });
    }
    if (use_ppmd_) {
      ppmd_f = std::async(std::launch::async, [&] { return make_ppmd_backend()->compress(request); });
    }
    if (use_bsc_) {
      bsc_f = std::async(std::launch::async, [&] { return make_bsc_backend()->compress(request); });
    }

    bool have_winner = false;
    CompressionResponse best;
    std::byte best_tag{0};
    const auto consider = [&](std::future<CompressionResponse>& f, std::byte tag) {
      if (!f.valid()) {
        return;
      }
      auto response = f.get();
      if (!have_winner || response.encoded.size() < best.encoded.size()) {
        best = std::move(response);
        best_tag = tag;
        have_winner = true;
      }
    };

    consider(lzma2_f, kTagLzma2);
    consider(zpaq_f, kTagZpaq);
    consider(ppmd_f, kTagPpmd);
    consider(bsc_f, kTagBsc);

    if (!have_winner) {
      throw std::runtime_error("best-of-n: no codec produced output");
    }

    std::vector<std::byte> tagged;
    tagged.reserve(1 + best.encoded.size());
    tagged.push_back(best_tag);
    tagged.insert(tagged.end(), best.encoded.begin(), best.encoded.end());
    best.encoded = std::move(tagged);
    return best;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) {
      throw std::runtime_error("best-of-n: empty payload");
    }
    const auto tag = encoded.front();
    const auto payload = encoded.subspan(1);

    if (tag == kTagLzma2) {
      return make_lzma2_backend()->decompress(payload, expected_raw_size);
    }
    if (tag == kTagZpaq) {
      return make_libzpaq_backend("5")->decompress(payload, expected_raw_size);
    }
    if (tag == kTagPpmd) {
      return make_ppmd_backend()->decompress(payload, expected_raw_size);
    }
    if (tag == kTagBsc) {
      auto bsc = make_bsc_backend();
      if (!bsc) {
        throw std::runtime_error("best-of-n: archive needs libbsc but it is unavailable");
      }
      return bsc->decompress(payload, expected_raw_size);
    }
    throw std::runtime_error("best-of-n: unknown codec tag byte");
  }

 private:
  std::string spec_;
  bool use_lzma2_ = false;
  bool use_zpaq_ = false;
  bool use_ppmd_ = false;
  bool use_bsc_ = false;
  std::string zpaq_method_ = "5";
};

}  // namespace

std::unique_ptr<CompressionBackend> make_best_of_n_backend(const std::string& spec) {
  return std::make_unique<BestOfNBackend>(spec);
}

}  // namespace devzip
