#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace devzip {

// User-facing compression level. Higher levels trade time for smaller output by
// enabling more (and more expensive) preprocessing transforms and stronger
// backends. The container format is identical across levels.
enum class CompressionLevel : std::uint8_t {
  Fast = 0,
  Balanced = 1,
  Max = 2,
  Insane = 3,
};

// Toggles that control which preprocessing transforms the pipeline may apply at
// compress time. The default-constructed value preserves the historical
// behavior (BCJ + delta filters only), so existing callers and tests are
// unaffected; richer behavior is opted into via CompressionOptions::for_level.
struct CompressionOptions {
  CompressionLevel level = CompressionLevel::Fast;

  bool enable_bcj = true;             // x86/x64 branch filter for PE binaries
  bool enable_delta = true;           // byte-delta filter for raw bitmaps
  bool enable_code_dictionary = false;  // token dictionary pre-pass (text/code)
  bool enable_png_idat_strip = false;   // inflate PNG IDAT, recompress with backend
  bool enable_brunsli = false;          // lossless JPEG transcode (Phase 2)
  bool enable_preflate = false;         // deflate-undo for PNG/ZIP/GZIP (Phase 3)

  // When true, every backend-compressed block is decoded and compared against
  // its plaintext at create time. Guarantees byte-exactness up front at the
  // cost of roughly doubling compression time. Risky transforms always
  // self-verify regardless of this flag.
  bool verify_roundtrip = false;

  static CompressionOptions for_level(CompressionLevel level) {
    CompressionOptions options;
    options.level = level;
    switch (level) {
      case CompressionLevel::Fast:
        // Filters only; skip the expensive recompressors. Fastest path.
        break;
      case CompressionLevel::Balanced:
        // brunsli is a cheap, huge JPEG win and the code dictionary / PNG IDAT
        // strip are inexpensive. preflate stays off here: undoing deflate then
        // deeply recompressing raw pixels is the single slowest transform, so it
        // is reserved for max/insane where time is explicitly traded for size.
        options.enable_png_idat_strip = true;
        options.enable_brunsli = true;
        options.enable_code_dictionary = true;
        break;
      case CompressionLevel::Max:
        options.enable_png_idat_strip = true;
        options.enable_brunsli = true;
        options.enable_preflate = true;
        options.enable_code_dictionary = true;
        break;
      case CompressionLevel::Insane:
        options.enable_png_idat_strip = true;
        options.enable_brunsli = true;
        options.enable_preflate = true;
        options.enable_code_dictionary = true;
        options.verify_roundtrip = true;
        break;
    }
    return options;
  }
};

inline CompressionLevel parse_compression_level(std::string_view name) {
  if (name == "fast") return CompressionLevel::Fast;
  if (name == "balanced") return CompressionLevel::Balanced;
  if (name == "max") return CompressionLevel::Max;
  if (name == "insane") return CompressionLevel::Insane;
  throw std::runtime_error("Unknown compression level: " + std::string(name));
}

inline std::string_view to_string(CompressionLevel level) {
  switch (level) {
    case CompressionLevel::Fast:
      return "fast";
    case CompressionLevel::Balanced:
      return "balanced";
    case CompressionLevel::Max:
      return "max";
    case CompressionLevel::Insane:
      return "insane";
  }
  return "balanced";
}

}  // namespace devzip
