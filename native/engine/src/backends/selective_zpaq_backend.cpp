#include "devzip/backend.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string_view>

namespace devzip {
namespace {

std::string lower_ascii(std::string_view value) {
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
  });
  return output;
}

bool prefers_higher_zpaq(std::string_view content_name) {
  const auto extension = lower_ascii(std::filesystem::path(std::string(content_name)).extension().string());
  static const std::array<std::string_view, 33> kTextAndRawExtensions = {
      ".bmp",  ".c",    ".cc",   ".cpp",  ".csv",  ".cs",   ".css",  ".go",   ".h",
      ".hpp",  ".html", ".ini",  ".java", ".js",   ".json", ".jsx",  ".kt",   ".log",
      ".md",   ".pbm",  ".pcx",  ".pgm",  ".ppm",  ".py",   ".raw",  ".rb",   ".rs",
      ".sql",  ".svg",  ".tga",  ".toml", ".txt",  ".xml",
  };
  return std::find(kTextAndRawExtensions.begin(), kTextAndRawExtensions.end(), extension) !=
         kTextAndRawExtensions.end();
}

class SelectiveZpaqBackend final : public CompressionBackend {
 public:
  SelectiveZpaqBackend()
      : default_backend_(make_best_of_two_backend("4")),
        high_ratio_backend_(make_best_of_two_backend("5")) {}

  BackendStamp stamp() const override { return {"selective-zpaq", "text-raw:m5,other:m4"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    if (prefers_higher_zpaq(request.content_name)) {
      return high_ratio_backend_->compress(request);
    }
    return default_backend_->compress(request);
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    return default_backend_->decompress(encoded, expected_raw_size);
  }

 private:
  std::unique_ptr<CompressionBackend> default_backend_;
  std::unique_ptr<CompressionBackend> high_ratio_backend_;
};

}  // namespace

std::unique_ptr<CompressionBackend> make_selective_zpaq_backend() {
  return std::make_unique<SelectiveZpaqBackend>();
}

}  // namespace devzip
