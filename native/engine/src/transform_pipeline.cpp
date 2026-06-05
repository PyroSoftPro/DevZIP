#include "devzip/transform_pipeline.h"

extern "C" {
#include "Bra.h"
#include "miniz.h"
}

#if defined(DEVZIP_HAVE_BRUNSLI)
extern "C" {
#include <brunsli/decode.h>
#include <brunsli/encode.h>
}
#endif

#if defined(DEVZIP_HAVE_PREFLATE)
#include "preflate_decoder.h"
#include "preflate_reencoder.h"
#include "support/memstream.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace devzip {
namespace {

bool starts_with_bytes(std::span<const std::byte> input, std::initializer_list<std::uint8_t> prefix) {
  if (input.size() < prefix.size()) {
    return false;
  }

  std::size_t index = 0;
  for (const auto byte : prefix) {
    if (static_cast<std::uint8_t>(input[index]) != byte) {
      return false;
    }
    ++index;
  }
  return true;
}

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

bool has_precompressed_signature(std::span<const std::byte> input) {
  const auto byte_char = [&input](std::size_t index) {
    return static_cast<char>(std::to_integer<unsigned char>(input[index]));
  };

  if (starts_with_bytes(input, {'O', 'g', 'g', 'S'}) ||
      starts_with_bytes(input, {'I', 'D', '3'})) {
    return true;
  }

  if (input.size() >= 12 && starts_with_bytes(input, {'R', 'I', 'F', 'F'}) &&
      byte_char(8) == 'W' && byte_char(9) == 'E' && byte_char(10) == 'B' && byte_char(11) == 'P') {
    return true;
  }

  if (input.size() >= 12 && byte_char(4) == 'f' && byte_char(5) == 't' &&
      byte_char(6) == 'y' && byte_char(7) == 'p') {
    return true;
  }

  // GIF87a / GIF89a
  if (starts_with_bytes(input, {0x47, 0x49, 0x46, 0x38})) {
    return true;
  }

  // 7-Zip
  if (starts_with_bytes(input, {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C})) {
    return true;
  }

  // RAR4 and RAR5
  if (starts_with_bytes(input, {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07})) {
    return true;
  }

  // bzip2
  if (starts_with_bytes(input, {0x42, 0x5A, 0x68})) {
    return true;
  }

  // xz
  if (starts_with_bytes(input, {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00})) {
    return true;
  }

  // Zstandard
  if (starts_with_bytes(input, {0x28, 0xB5, 0x2F, 0xFD})) {
    return true;
  }

  return false;
}

bool prefers_stored_raw(std::span<const std::byte> input, std::string_view archive_path) {
  const auto extension = lower_ascii(std::filesystem::path(std::string(archive_path)).extension().string());
  static const std::array<std::string_view, 17> kRawExtensions = {
      ".aac",  ".avi",   ".bz2",  ".cbr",  ".flac",
      ".gif",  ".gz",    ".m4a",  ".mkv",  ".mov",
      ".mp3",  ".mp4",   ".rar",  ".woff", ".woff2",
      ".xz",   ".zst",
  };

  if (std::find(kRawExtensions.begin(), kRawExtensions.end(), extension) != kRawExtensions.end()) {
    return true;
  }

  return has_precompressed_signature(input);
}

bool should_chunk_file(std::span<const std::byte> input,
                       std::string_view archive_path,
                       bool prefer_raw,
                       bool text_like_input) {
  constexpr std::size_t kChunkWorthwhileBytes = 64 * 1024;

  if (input.size() <= kChunkWorthwhileBytes || prefer_raw) {
    return false;
  }

  if (text_like_input) {
    return true;
  }

  const auto extension = lower_ascii(std::filesystem::path(std::string(archive_path)).extension().string());
  static const std::array<std::string_view, 5> kStructuredTextExtensions = {
      ".csv", ".yaml", ".yml", ".svg", ".log",
  };
  return std::find(kStructuredTextExtensions.begin(), kStructuredTextExtensions.end(), extension) !=
         kStructuredTextExtensions.end();
}

bool is_pe_executable(std::span<const std::byte> input, std::string_view archive_path) {
  const auto extension = lower_ascii(std::filesystem::path(std::string(archive_path)).extension().string());
  static const std::array<std::string_view, 4> kExeExtensions = {".exe", ".dll", ".sys", ".ocx"};
  if (std::find(kExeExtensions.begin(), kExeExtensions.end(), extension) != kExeExtensions.end()) {
    return true;
  }
  return starts_with_bytes(input, {0x4D, 0x5A});
}

// Which branch converter (if any) helps a given executable.  The LZMA SDK X86
// converter only models 32-bit x86 CALL/JMP encodings well; applied to x64 it
// adds noise (high E8/E9 false-positive density) and measurably *hurts* ratio.
// We therefore read the PE COFF machine word and pick the matching converter,
// falling back to "no filter" for architectures we cannot improve.
enum class BranchFilter { None, X86, Arm64, Arm, ArmThumb };

BranchFilter select_branch_filter(std::span<const std::byte> input) {
  // DOS header: 'MZ' + e_lfanew (LE u32) at offset 0x3C points at the PE header.
  if (input.size() < 0x40 || !starts_with_bytes(input, {0x4D, 0x5A})) {
    return BranchFilter::None;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.data());
  const std::uint32_t pe_offset = static_cast<std::uint32_t>(bytes[0x3C]) |
                                  (static_cast<std::uint32_t>(bytes[0x3D]) << 8) |
                                  (static_cast<std::uint32_t>(bytes[0x3E]) << 16) |
                                  (static_cast<std::uint32_t>(bytes[0x3F]) << 24);
  // Need 'PE\0\0' (4) + machine word (2).
  if (static_cast<std::size_t>(pe_offset) + 6 > input.size()) {
    return BranchFilter::None;
  }
  if (bytes[pe_offset] != 'P' || bytes[pe_offset + 1] != 'E' ||
      bytes[pe_offset + 2] != 0 || bytes[pe_offset + 3] != 0) {
    return BranchFilter::None;
  }
  const std::uint16_t machine = static_cast<std::uint16_t>(bytes[pe_offset + 4]) |
                                (static_cast<std::uint16_t>(bytes[pe_offset + 5]) << 8);
  switch (machine) {
    case 0x014C:  // IMAGE_FILE_MACHINE_I386
      return BranchFilter::X86;
    case 0xAA64:  // IMAGE_FILE_MACHINE_ARM64
      return BranchFilter::Arm64;
    case 0x01C0:  // IMAGE_FILE_MACHINE_ARM
      return BranchFilter::Arm;
    case 0x01C4:  // IMAGE_FILE_MACHINE_ARMNT (Thumb-2)
      return BranchFilter::ArmThumb;
    case 0x8664:  // IMAGE_FILE_MACHINE_AMD64 - X86 CALL/JMP filter still helps.
      return BranchFilter::X86;
    default:
      return BranchFilter::None;
  }
}

bool is_jpeg(std::span<const std::byte> input, std::string_view archive_path) {
  if (starts_with_bytes(input, {0xFF, 0xD8, 0xFF})) {
    return true;
  }
  const auto extension = lower_ascii(
      std::filesystem::path(std::string(archive_path)).extension().string());
  return extension == ".jpg" || extension == ".jpeg" || extension == ".jpe";
}

bool is_delta_candidate(std::string_view archive_path) {
  const auto extension = lower_ascii(std::filesystem::path(std::string(archive_path)).extension().string());
  static const std::array<std::string_view, 7> kDeltaExtensions = {
      ".bmp", ".tga", ".raw", ".pcx", ".ppm", ".pgm", ".pbm",
  };
  return std::find(kDeltaExtensions.begin(), kDeltaExtensions.end(), extension) != kDeltaExtensions.end();
}

// ---------------------------------------------------------------------------
// PNG chunk helpers
// ---------------------------------------------------------------------------

static constexpr std::array<std::uint8_t, 8> kPngSignature = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

static std::uint32_t png_u32be(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

static void write_u32be(std::vector<std::byte>& out, std::uint32_t v) {
  out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
  out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<std::byte>(v & 0xFF));
}

static std::uint32_t crc32_for(const std::uint8_t* data, std::size_t len) {
  return static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, data, len));
}

struct PngChunk {
  std::uint32_t type = 0;
  std::vector<std::uint8_t> data;
  std::uint32_t crc = 0;
};

static bool is_idat(std::uint32_t type) {
  return type == 0x49444154u;  // "IDAT"
}

static std::vector<PngChunk> parse_png_chunks(std::span<const std::byte> input) {
  if (input.size() < 8 ||
      std::memcmp(input.data(), kPngSignature.data(), 8) != 0) {
    return {};
  }

  std::vector<PngChunk> chunks;
  std::size_t pos = 8;
  while (pos + 12 <= input.size()) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(input.data()) + pos;
    const std::uint32_t data_len = png_u32be(p);
    if (pos + 12 + data_len > input.size()) {
      return {};
    }
    const std::uint32_t type = png_u32be(p + 4);
    const std::uint32_t stored_crc = png_u32be(p + 8 + data_len);

    PngChunk chunk;
    chunk.type = type;
    chunk.data.assign(p + 8, p + 8 + data_len);
    chunk.crc = stored_crc;
    chunks.push_back(std::move(chunk));
    pos += 12 + data_len;

    if (type == 0x49454E44u) {  // IEND
      break;
    }
  }
  return chunks;
}

// Try to inflate zlib-wrapped data (PNG IDAT is a zlib stream).
static bool png_inflate(const std::vector<std::uint8_t>& compressed,
                        std::vector<std::uint8_t>& raw_out) {
  mz_ulong out_size = static_cast<mz_ulong>(compressed.size()) * 8;
  if (out_size < 1024) out_size = 1024;

  for (int attempts = 0; attempts < 4; ++attempts) {
    raw_out.resize(static_cast<std::size_t>(out_size));
    mz_ulong actual = out_size;
    const int rc = mz_uncompress(raw_out.data(), &actual, compressed.data(),
                                 static_cast<mz_ulong>(compressed.size()));
    if (rc == MZ_OK) {
      raw_out.resize(static_cast<std::size_t>(actual));
      return true;
    }
    if (rc == MZ_BUF_ERROR) {
      if (out_size > 2ull * 1024 * 1024 * 1024) {
        return false;
      }
      out_size *= 4;
      continue;
    }
    return false;
  }
  return false;
}

// Try to deflate raw_pixels and compare with original.  Returns the
// compression level that successfully reproduced the original bytes, or -1.
static int png_try_deflate_match(const std::vector<std::uint8_t>& raw,
                                 const std::vector<std::uint8_t>& original) {
  // Try common libpng compression levels (9 and 6 are most common).
  static const int kLevels[] = {9, 6};
  for (const int level : kLevels) {
    mz_ulong out_size =
        mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(out_size));
    mz_ulong actual = out_size;
    if (mz_compress2(buf.data(), &actual, raw.data(),
                     static_cast<mz_ulong>(raw.size()), level) != MZ_OK) {
      continue;
    }
    buf.resize(static_cast<std::size_t>(actual));
    if (buf == original) {
      return level;
    }
  }
  return -1;
}

// Serialise/deserialise the PNG recipe.
// Layout (all integers little-endian):
//   [4]  magic 0x504E4749
//   [4]  deflate_level
//   [4]  num_idat_chunks
//   [4 * num_idat] each original IDAT data-length
//   [4]  pre_idat_byte_count
//   [pre_idat_byte_count] bytes before first IDAT (signature + IHDR + ancillary)
//   [4]  post_idat_byte_count
//   [post_idat_byte_count] bytes after last IDAT (IEND + trailing)

struct PngRecipe {
  int deflate_level = 9;
  std::vector<std::uint32_t> idat_sizes;
  std::vector<std::byte> pre_idat;
  std::vector<std::byte> post_idat;
};

static void write_le32(std::string& s, std::uint32_t v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>((v >> 16) & 0xFF));
  s.push_back(static_cast<char>((v >> 24) & 0xFF));
}

static std::uint32_t read_le32(const char* p) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

static std::string serialise_png_recipe(const PngRecipe& r) {
  std::string out;
  write_le32(out, 0x504E4749u);
  write_le32(out, static_cast<std::uint32_t>(r.deflate_level));
  write_le32(out, static_cast<std::uint32_t>(r.idat_sizes.size()));
  for (const auto sz : r.idat_sizes) {
    write_le32(out, sz);
  }
  write_le32(out, static_cast<std::uint32_t>(r.pre_idat.size()));
  out.append(reinterpret_cast<const char*>(r.pre_idat.data()), r.pre_idat.size());
  write_le32(out, static_cast<std::uint32_t>(r.post_idat.size()));
  out.append(reinterpret_cast<const char*>(r.post_idat.data()), r.post_idat.size());
  return out;
}

static bool deserialise_png_recipe(const std::string& s, PngRecipe& out) {
  const char* p = s.data();
  const char* end = p + s.size();
  auto need = [&](std::size_t n) { return (end - p) >= static_cast<std::ptrdiff_t>(n); };

  if (!need(4) || read_le32(p) != 0x504E4749u) return false;
  p += 4;
  if (!need(4)) return false;
  out.deflate_level = static_cast<int>(read_le32(p)); p += 4;
  if (!need(4)) return false;
  const std::uint32_t num_idat = read_le32(p); p += 4;
  if (!need(4ull * num_idat)) return false;
  out.idat_sizes.resize(num_idat);
  for (auto& sz : out.idat_sizes) {
    sz = read_le32(p); p += 4;
  }
  if (!need(4)) return false;
  const std::uint32_t pre_len = read_le32(p); p += 4;
  if (!need(pre_len)) return false;
  out.pre_idat.assign(reinterpret_cast<const std::byte*>(p),
                      reinterpret_cast<const std::byte*>(p) + pre_len);
  p += pre_len;
  if (!need(4)) return false;
  const std::uint32_t post_len = read_le32(p); p += 4;
  if (!need(post_len)) return false;
  out.post_idat.assign(reinterpret_cast<const std::byte*>(p),
                       reinterpret_cast<const std::byte*>(p) + post_len);
  return true;
}

}  // namespace

namespace transforms {

std::vector<std::byte> apply_bcj_x86(std::span<const std::byte> input) {
  if (input.empty()) {
    return {};
  }
  std::vector<std::byte> output(input.begin(), input.end());
  UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
  z7_BranchConvSt_X86_Enc(reinterpret_cast<Byte*>(output.data()),
                           static_cast<SizeT>(output.size()), 0, &state);
  return output;
}

std::vector<std::byte> reverse_bcj_x86(std::span<const std::byte> input) {
  if (input.empty()) {
    return {};
  }
  std::vector<std::byte> output(input.begin(), input.end());
  UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
  z7_BranchConvSt_X86_Dec(reinterpret_cast<Byte*>(output.data()),
                           static_cast<SizeT>(output.size()), 0, &state);
  return output;
}

// RISC branch converters (ARM64/ARM/ARMT) are stateless: they rewrite CALL/BL
// relative targets to absolute so the entropy coder sees repeated address bytes.
std::vector<std::byte> apply_bcj_arm64(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARM64_Enc(reinterpret_cast<Byte*>(output.data()),
                          static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> reverse_bcj_arm64(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARM64_Dec(reinterpret_cast<Byte*>(output.data()),
                          static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> apply_bcj_arm(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARM_Enc(reinterpret_cast<Byte*>(output.data()),
                        static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> reverse_bcj_arm(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARM_Dec(reinterpret_cast<Byte*>(output.data()),
                        static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> apply_bcj_arm_thumb(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARMT_Enc(reinterpret_cast<Byte*>(output.data()),
                         static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> reverse_bcj_arm_thumb(std::span<const std::byte> input) {
  if (input.empty()) return {};
  std::vector<std::byte> output(input.begin(), input.end());
  z7_BranchConv_ARMT_Dec(reinterpret_cast<Byte*>(output.data()),
                         static_cast<SizeT>(output.size()), 0);
  return output;
}

std::vector<std::byte> apply_delta_filter(std::span<const std::byte> input) {
  if (input.empty()) {
    return {};
  }
  std::vector<std::byte> output(input.size());
  output[0] = input[0];
  for (std::size_t i = 1; i < input.size(); ++i) {
    output[i] = static_cast<std::byte>(
        static_cast<std::uint8_t>(input[i]) - static_cast<std::uint8_t>(input[i - 1]));
  }
  return output;
}

std::vector<std::byte> reverse_delta_filter(std::span<const std::byte> input) {
  if (input.empty()) {
    return {};
  }
  std::vector<std::byte> output(input.size());
  output[0] = input[0];
  for (std::size_t i = 1; i < input.size(); ++i) {
    output[i] = static_cast<std::byte>(
        static_cast<std::uint8_t>(input[i]) + static_cast<std::uint8_t>(output[i - 1]));
  }
  return output;
}

// Returns an empty vector if the transform cannot be applied losslessly.
std::vector<std::byte> apply_png_idat_strip(
    std::span<const std::byte> input, std::string& recipe_out) {
  const auto chunks = parse_png_chunks(input);
  if (chunks.empty()) return {};

  std::vector<std::uint8_t> combined_idat;
  std::vector<std::uint32_t> idat_sizes;
  std::vector<std::byte> pre_idat_bytes;
  std::vector<std::byte> post_idat_bytes;
  bool seen_idat = false;

  auto emit_chunk = [](std::vector<std::byte>& dest, const PngChunk& ch) {
    const std::uint32_t len = static_cast<std::uint32_t>(ch.data.size());
    write_u32be(dest, len);
    dest.push_back(static_cast<std::byte>((ch.type >> 24) & 0xFF));
    dest.push_back(static_cast<std::byte>((ch.type >> 16) & 0xFF));
    dest.push_back(static_cast<std::byte>((ch.type >> 8) & 0xFF));
    dest.push_back(static_cast<std::byte>(ch.type & 0xFF));
    for (const auto b : ch.data) {
      dest.push_back(static_cast<std::byte>(b));
    }
    write_u32be(dest, ch.crc);
  };

  for (const auto b : kPngSignature) {
    pre_idat_bytes.push_back(static_cast<std::byte>(b));
  }

  for (const auto& ch : chunks) {
    if (!seen_idat && !is_idat(ch.type)) {
      emit_chunk(pre_idat_bytes, ch);
      continue;
    }
    if (is_idat(ch.type)) {
      seen_idat = true;
      idat_sizes.push_back(static_cast<std::uint32_t>(ch.data.size()));
      combined_idat.insert(combined_idat.end(), ch.data.begin(), ch.data.end());
      continue;
    }
    emit_chunk(post_idat_bytes, ch);
  }

  if (combined_idat.empty()) return {};

  std::vector<std::uint8_t> raw_pixels;
  if (!png_inflate(combined_idat, raw_pixels)) return {};

  const int level = png_try_deflate_match(raw_pixels, combined_idat);
  if (level < 0) return {};

  PngRecipe recipe;
  recipe.deflate_level = level;
  recipe.idat_sizes = std::move(idat_sizes);
  recipe.pre_idat = std::move(pre_idat_bytes);
  recipe.post_idat = std::move(post_idat_bytes);
  recipe_out = serialise_png_recipe(recipe);

  std::vector<std::byte> result(raw_pixels.size());
  std::memcpy(result.data(), raw_pixels.data(), raw_pixels.size());
  return result;
}

std::vector<std::byte> reverse_png_idat_strip(
    std::span<const std::byte> raw_pixels, const std::string& recipe_str) {
  PngRecipe recipe;
  if (!deserialise_png_recipe(recipe_str, recipe)) {
    throw std::runtime_error("PNG IDAT strip: corrupt recipe");
  }

  mz_ulong out_bound =
      mz_compressBound(static_cast<mz_ulong>(raw_pixels.size()));
  std::vector<std::uint8_t> compressed(static_cast<std::size_t>(out_bound));
  mz_ulong actual = out_bound;
  if (mz_compress2(compressed.data(), &actual,
                   reinterpret_cast<const unsigned char*>(raw_pixels.data()),
                   static_cast<mz_ulong>(raw_pixels.size()),
                   recipe.deflate_level) != MZ_OK) {
    throw std::runtime_error("PNG IDAT strip: re-deflate failed during extraction");
  }
  compressed.resize(static_cast<std::size_t>(actual));

  std::vector<std::byte> png_out;
  png_out.reserve(recipe.pre_idat.size() + compressed.size() +
                  recipe.idat_sizes.size() * 12 + recipe.post_idat.size() + 8);
  png_out.insert(png_out.end(), recipe.pre_idat.begin(), recipe.pre_idat.end());

  static const std::uint8_t kIdatType[4] = {'I', 'D', 'A', 'T'};
  std::size_t comp_pos = 0;
  for (const std::uint32_t idat_size : recipe.idat_sizes) {
    if (comp_pos + idat_size > compressed.size()) {
      throw std::runtime_error("PNG IDAT strip: compressed stream shorter than expected");
    }
    const auto* chunk_data = compressed.data() + comp_pos;
    std::uint32_t c = crc32_for(kIdatType, 4);
    c = static_cast<std::uint32_t>(
        mz_crc32(static_cast<mz_ulong>(c), chunk_data, idat_size));

    write_u32be(png_out, idat_size);
    for (const auto b : kIdatType) {
      png_out.push_back(static_cast<std::byte>(b));
    }
    for (std::uint32_t i = 0; i < idat_size; ++i) {
      png_out.push_back(static_cast<std::byte>(chunk_data[i]));
    }
    write_u32be(png_out, c);
    comp_pos += idat_size;
  }

  png_out.insert(png_out.end(), recipe.post_idat.begin(), recipe.post_idat.end());
  return png_out;
}

// ---------------------------------------------------------------------------
// brunsli JPEG transcoding
// ---------------------------------------------------------------------------
#if defined(DEVZIP_HAVE_BRUNSLI)
namespace {
size_t brunsli_sink(void* out_data, const std::uint8_t* buf, size_t size) {
  auto* sink = static_cast<std::vector<std::byte>*>(out_data);
  const auto* begin = reinterpret_cast<const std::byte*>(buf);
  sink->insert(sink->end(), begin, begin + size);
  return size;
}
}  // namespace

bool brunsli_available() { return true; }

std::vector<std::byte> apply_jpeg_brunsli(std::span<const std::byte> input) {
  if (input.size() < 3) {
    return {};
  }
  std::vector<std::byte> output;
  output.reserve(input.size() / 2 + 64);
  const int ok = EncodeBrunsli(input.size(),
                               reinterpret_cast<const unsigned char*>(input.data()),
                               &output, &brunsli_sink);
  if (ok != 1 || output.empty()) {
    return {};
  }
  return output;
}

std::vector<std::byte> reverse_jpeg_brunsli(std::span<const std::byte> input,
                                            std::size_t expected_size) {
  std::vector<std::byte> output;
  output.reserve(expected_size ? expected_size : input.size() * 2 + 64);
  const int ok = DecodeBrunsli(input.size(),
                               reinterpret_cast<const std::uint8_t*>(input.data()),
                               &output, &brunsli_sink);
  if (ok != 1) {
    throw std::runtime_error("brunsli failed to reconstruct JPEG stream");
  }
  return output;
}
#else
bool brunsli_available() { return false; }
std::vector<std::byte> apply_jpeg_brunsli(std::span<const std::byte>) { return {}; }
std::vector<std::byte> reverse_jpeg_brunsli(std::span<const std::byte>, std::size_t) {
  throw std::runtime_error("brunsli support was not compiled in");
}
#endif

// ---------------------------------------------------------------------------
// preflate PNG recompression
// ---------------------------------------------------------------------------
#if defined(DEVZIP_HAVE_PREFLATE)

// Recipe layout (little-endian):
//   [4]  magic 0x50464C54 ("PFLT")
//   [1]  zlib CMF
//   [1]  zlib FLG
//   [4]  adler32 (as stored, big-endian-order bytes copied verbatim)
//   [4]  num_idat ; [4*num_idat] original IDAT chunk sizes
//   [4]  pre_idat len ; [..] pre_idat bytes
//   [4]  post_idat len ; [..] post_idat bytes
//   [4]  diff len ; [..] preflate reconstruction diff
bool preflate_available() { return true; }

std::vector<std::byte> apply_png_preflate(std::span<const std::byte> input,
                                          std::string& recipe_out) {
  const auto chunks = parse_png_chunks(input);
  if (chunks.empty()) return {};

  std::vector<std::uint8_t> combined_idat;
  std::vector<std::uint32_t> idat_sizes;
  std::vector<std::byte> pre_idat;
  std::vector<std::byte> post_idat;
  bool seen_idat = false;

  auto emit_chunk = [](std::vector<std::byte>& dest, const PngChunk& ch) {
    write_u32be(dest, static_cast<std::uint32_t>(ch.data.size()));
    dest.push_back(static_cast<std::byte>((ch.type >> 24) & 0xFF));
    dest.push_back(static_cast<std::byte>((ch.type >> 16) & 0xFF));
    dest.push_back(static_cast<std::byte>((ch.type >> 8) & 0xFF));
    dest.push_back(static_cast<std::byte>(ch.type & 0xFF));
    for (const auto b : ch.data) dest.push_back(static_cast<std::byte>(b));
    write_u32be(dest, ch.crc);
  };

  for (const auto b : kPngSignature) pre_idat.push_back(static_cast<std::byte>(b));
  for (const auto& ch : chunks) {
    if (is_idat(ch.type)) {
      seen_idat = true;
      idat_sizes.push_back(static_cast<std::uint32_t>(ch.data.size()));
      combined_idat.insert(combined_idat.end(), ch.data.begin(), ch.data.end());
    } else if (!seen_idat) {
      emit_chunk(pre_idat, ch);
    } else {
      emit_chunk(post_idat, ch);
    }
  }

  // zlib stream = [CMF][FLG][deflate...][ADLER32]; need at least that framing.
  if (combined_idat.size() < 7) return {};

  std::vector<std::uint8_t> deflate_plus(combined_idat.begin() + 2, combined_idat.end());
  MemStream mem(deflate_plus);
  std::vector<unsigned char> unpacked;
  std::vector<unsigned char> diff;
  std::uint64_t deflate_size = 0;
  const bool ok = preflate_decode(unpacked, diff, deflate_size, mem, [] {}, 0, INT32_MAX);
  if (!ok || diff.empty()) return {};
  // The 4 bytes following the deflate stream are the zlib adler32 checksum.
  if (static_cast<std::size_t>(2 + deflate_size + 4) != combined_idat.size()) return {};

  std::string recipe;
  write_le32(recipe, 0x50464C54u);
  recipe.push_back(static_cast<char>(combined_idat[0]));
  recipe.push_back(static_cast<char>(combined_idat[1]));
  for (std::size_t i = 0; i < 4; ++i) {
    recipe.push_back(static_cast<char>(combined_idat[2 + deflate_size + i]));
  }
  write_le32(recipe, static_cast<std::uint32_t>(idat_sizes.size()));
  for (const auto sz : idat_sizes) write_le32(recipe, sz);
  write_le32(recipe, static_cast<std::uint32_t>(pre_idat.size()));
  recipe.append(reinterpret_cast<const char*>(pre_idat.data()), pre_idat.size());
  write_le32(recipe, static_cast<std::uint32_t>(post_idat.size()));
  recipe.append(reinterpret_cast<const char*>(post_idat.data()), post_idat.size());
  write_le32(recipe, static_cast<std::uint32_t>(diff.size()));
  recipe.append(reinterpret_cast<const char*>(diff.data()), diff.size());
  recipe_out = std::move(recipe);

  std::vector<std::byte> result(unpacked.size());
  if (!unpacked.empty()) std::memcpy(result.data(), unpacked.data(), unpacked.size());
  return result;
}

std::vector<std::byte> reverse_png_preflate(std::span<const std::byte> raw_pixels,
                                            const std::string& recipe_str) {
  const char* p = recipe_str.data();
  const char* end = p + recipe_str.size();
  auto need = [&](std::size_t n) { return (end - p) >= static_cast<std::ptrdiff_t>(n); };

  if (!need(4) || read_le32(p) != 0x50464C54u) {
    throw std::runtime_error("PNG preflate: corrupt recipe magic");
  }
  p += 4;
  if (!need(6)) throw std::runtime_error("PNG preflate: truncated header");
  const std::uint8_t cmf = static_cast<std::uint8_t>(*p++);
  const std::uint8_t flg = static_cast<std::uint8_t>(*p++);
  std::uint8_t adler[4];
  for (auto& a : adler) a = static_cast<std::uint8_t>(*p++);
  if (!need(4)) throw std::runtime_error("PNG preflate: truncated idat table");
  const std::uint32_t num_idat = read_le32(p); p += 4;
  if (!need(4ull * num_idat)) throw std::runtime_error("PNG preflate: bad idat table");
  std::vector<std::uint32_t> idat_sizes(num_idat);
  for (auto& sz : idat_sizes) { sz = read_le32(p); p += 4; }
  if (!need(4)) throw std::runtime_error("PNG preflate: truncated pre");
  const std::uint32_t pre_len = read_le32(p); p += 4;
  if (!need(pre_len)) throw std::runtime_error("PNG preflate: truncated pre bytes");
  const char* pre_ptr = p; p += pre_len;
  if (!need(4)) throw std::runtime_error("PNG preflate: truncated post");
  const std::uint32_t post_len = read_le32(p); p += 4;
  if (!need(post_len)) throw std::runtime_error("PNG preflate: truncated post bytes");
  const char* post_ptr = p; p += post_len;
  if (!need(4)) throw std::runtime_error("PNG preflate: truncated diff");
  const std::uint32_t diff_len = read_le32(p); p += 4;
  if (!need(diff_len)) throw std::runtime_error("PNG preflate: truncated diff bytes");
  std::vector<unsigned char> diff(reinterpret_cast<const unsigned char*>(p),
                                  reinterpret_cast<const unsigned char*>(p) + diff_len);

  std::vector<unsigned char> unpacked(raw_pixels.size());
  if (!raw_pixels.empty()) std::memcpy(unpacked.data(), raw_pixels.data(), raw_pixels.size());

  std::vector<unsigned char> deflate_body;
  if (!preflate_reencode(deflate_body, diff, unpacked)) {
    throw std::runtime_error("PNG preflate: failed to reconstruct deflate stream");
  }

  std::vector<std::uint8_t> combined_idat;
  combined_idat.reserve(deflate_body.size() + 6);
  combined_idat.push_back(cmf);
  combined_idat.push_back(flg);
  combined_idat.insert(combined_idat.end(), deflate_body.begin(), deflate_body.end());
  for (const auto a : adler) combined_idat.push_back(a);

  std::vector<std::byte> png_out;
  png_out.insert(png_out.end(), reinterpret_cast<const std::byte*>(pre_ptr),
                 reinterpret_cast<const std::byte*>(pre_ptr) + pre_len);

  static const std::uint8_t kIdatType[4] = {'I', 'D', 'A', 'T'};
  std::size_t pos = 0;
  for (const std::uint32_t idat_size : idat_sizes) {
    if (pos + idat_size > combined_idat.size()) {
      throw std::runtime_error("PNG preflate: rebuilt stream shorter than expected");
    }
    const auto* chunk_data = combined_idat.data() + pos;
    std::uint32_t c = crc32_for(kIdatType, 4);
    c = static_cast<std::uint32_t>(mz_crc32(static_cast<mz_ulong>(c), chunk_data, idat_size));
    write_u32be(png_out, idat_size);
    for (const auto b : kIdatType) png_out.push_back(static_cast<std::byte>(b));
    for (std::uint32_t i = 0; i < idat_size; ++i) {
      png_out.push_back(static_cast<std::byte>(chunk_data[i]));
    }
    write_u32be(png_out, c);
    pos += idat_size;
  }
  if (pos != combined_idat.size()) {
    throw std::runtime_error("PNG preflate: idat sizes do not cover the stream");
  }

  png_out.insert(png_out.end(), reinterpret_cast<const std::byte*>(post_ptr),
                 reinterpret_cast<const std::byte*>(post_ptr) + post_len);
  return png_out;
}
#else
bool preflate_available() { return false; }
std::vector<std::byte> apply_png_preflate(std::span<const std::byte>, std::string&) { return {}; }
std::vector<std::byte> reverse_png_preflate(std::span<const std::byte>, const std::string&) {
  throw std::runtime_error("preflate support was not compiled in");
}
#endif

}  // namespace transforms

PreparedFile TransformPipeline::prepare_file(std::span<const std::byte> input,
                                             std::string_view archive_path) const {
  PreparedFile prepared;
  prepared.content_checksum = checksum_bytes(input);

  std::string stream_recipe;
  const auto normalized = transforms::apply_stream_normalization(input, stream_recipe);
  if (!stream_recipe.empty()) {
    PreparedBlock block;
    block.payload = normalized;
    block.fallback_payload.assign(input.begin(), input.end());
    block.payload_checksum = checksum_bytes(block.payload);
    block.fallback_checksum = checksum_bytes(block.fallback_payload);
    block.transforms = {TransformKind::StreamNormalization};
    block.recipe = stream_recipe;
    prepared.blocks.push_back(std::move(block));
    return prepared;
  }

  const bool prefer_raw = prefers_stored_raw(input, archive_path);

  const bool is_png_input =
      starts_with_bytes(input, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});

  // PNG via preflate: undo the IDAT deflate to raw filtered pixel bytes plus a
  // reconstruction recipe.  Unlike the level-match IDAT strip below, preflate
  // reconstructs ANY deflate encoder's output bit-exactly, so it engages on the
  // PNGs that the strip path cannot handle.  We re-deflate immediately and
  // compare to guarantee a byte-exact roundtrip before committing.
  if (!prefer_raw && is_png_input && options_.enable_preflate &&
      transforms::preflate_available()) {
    std::string preflate_recipe;
    auto preflate_payload = transforms::apply_png_preflate(input, preflate_recipe);
    if (!preflate_payload.empty() && !preflate_recipe.empty()) {
      bool exact = false;
      try {
        const auto restored =
            transforms::reverse_png_preflate(preflate_payload, preflate_recipe);
        exact = restored.size() == input.size() &&
                std::memcmp(restored.data(), input.data(), input.size()) == 0;
      } catch (...) {
        exact = false;
      }
      if (exact) {
        PreparedBlock block;
        block.payload = std::move(preflate_payload);
        block.fallback_payload.assign(input.begin(), input.end());
        block.payload_checksum = checksum_bytes(block.payload);
        block.fallback_checksum = checksum_bytes(block.fallback_payload);
        block.transforms = {TransformKind::PreflateDeflate};
        block.recipe = std::move(preflate_recipe);
        prepared.blocks.push_back(std::move(block));
        return prepared;
      }
    }
  }

  // PNG IDAT strip: inflate the embedded deflate stream so the backend can model
  // the raw pixel bytes (which compress far better than already-deflated IDAT).
  // apply_png_idat_strip only returns data when it can re-deflate to the exact
  // original bytes, so the transform is always lossless / self-verifying.
  if (!prefer_raw && options_.enable_png_idat_strip && is_png_input) {
    std::string png_recipe;
    auto png_payload = transforms::apply_png_idat_strip(input, png_recipe);
    if (!png_payload.empty() && !png_recipe.empty()) {
      PreparedBlock block;
      block.payload = std::move(png_payload);
      block.fallback_payload.assign(input.begin(), input.end());
      block.payload_checksum = checksum_bytes(block.payload);
      block.fallback_checksum = checksum_bytes(block.fallback_payload);
      block.transforms = {TransformKind::PngIdatStrip};
      block.recipe = std::move(png_recipe);
      prepared.blocks.push_back(std::move(block));
      return prepared;
    }
  }

  // JPEG -> brunsli transcode.  brunsli is lossless, but not every JPEG variant
  // is supported, so we re-decode the transcoded stream and compare byte-for-byte
  // before committing; any mismatch falls through to the normal path.
  if (!prefer_raw && options_.enable_brunsli && transforms::brunsli_available() &&
      is_jpeg(input, archive_path)) {
    auto brunsli_payload = transforms::apply_jpeg_brunsli(input);
    if (!brunsli_payload.empty() && brunsli_payload.size() < input.size()) {
      bool exact = false;
      try {
        const auto restored = transforms::reverse_jpeg_brunsli(brunsli_payload, input.size());
        exact = restored.size() == input.size() &&
                std::memcmp(restored.data(), input.data(), input.size()) == 0;
      } catch (...) {
        exact = false;
      }
      if (exact) {
        PreparedBlock block;
        block.payload = std::move(brunsli_payload);
        block.fallback_payload.assign(input.begin(), input.end());
        block.payload_checksum = checksum_bytes(block.payload);
        block.fallback_checksum = checksum_bytes(block.fallback_payload);
        block.transforms = {TransformKind::JpegBrunsli};
        prepared.blocks.push_back(std::move(block));
        return prepared;
      }
    }
  }

  if (!prefer_raw && options_.enable_bcj && is_pe_executable(input, archive_path)) {
    const BranchFilter filter = select_branch_filter(input);
    if (filter != BranchFilter::None) {
      std::vector<std::byte> bcj_payload;
      TransformKind kind = TransformKind::BcjX86;
      switch (filter) {
        case BranchFilter::X86:
          bcj_payload = transforms::apply_bcj_x86(input);
          kind = TransformKind::BcjX86;
          break;
        case BranchFilter::Arm64:
          bcj_payload = transforms::apply_bcj_arm64(input);
          kind = TransformKind::BcjArm64;
          break;
        case BranchFilter::Arm:
          bcj_payload = transforms::apply_bcj_arm(input);
          kind = TransformKind::BcjArm;
          break;
        case BranchFilter::ArmThumb:
          bcj_payload = transforms::apply_bcj_arm_thumb(input);
          kind = TransformKind::BcjArmThumb;
          break;
        case BranchFilter::None:
          break;
      }
      PreparedBlock block;
      block.payload = std::move(bcj_payload);
      block.fallback_payload.assign(input.begin(), input.end());
      block.payload_checksum = checksum_bytes(block.payload);
      block.fallback_checksum = checksum_bytes(block.fallback_payload);
      block.transforms = {kind};
      prepared.blocks.push_back(std::move(block));
      return prepared;
    }
  }

  if (!prefer_raw && options_.enable_delta && is_delta_candidate(archive_path)) {
    auto delta_payload = transforms::apply_delta_filter(input);
    PreparedBlock block;
    block.payload = std::move(delta_payload);
    block.fallback_payload.assign(input.begin(), input.end());
    block.payload_checksum = checksum_bytes(block.payload);
    block.fallback_checksum = checksum_bytes(block.fallback_payload);
    block.transforms = {TransformKind::DeltaFilter};
    prepared.blocks.push_back(std::move(block));
    return prepared;
  }

  const bool text_like_input = transforms::looks_like_text(input);

  // Code dictionary: substitute frequent multi-byte tokens with short escapes
  // before the backend sees the bytes. apply_code_dictionary returns an empty
  // recipe (and the original bytes) when it cannot shrink the input.
  if (!prefer_raw && options_.enable_code_dictionary && text_like_input) {
    std::string code_recipe;
    auto code_payload = transforms::apply_code_dictionary(input, code_recipe);
    if (!code_recipe.empty() && code_payload.size() < input.size()) {
      PreparedBlock block;
      block.payload = std::move(code_payload);
      block.fallback_payload.assign(input.begin(), input.end());
      block.payload_checksum = checksum_bytes(block.payload);
      block.fallback_checksum = checksum_bytes(block.fallback_payload);
      block.transforms = {TransformKind::CodeDictionary};
      block.recipe = std::move(code_recipe);
      prepared.blocks.push_back(std::move(block));
      return prepared;
    }
  }

  if (should_chunk_file(input, archive_path, prefer_raw, text_like_input)) {
    const auto chunks = transforms::fastcdc_chunk_bytes(input);
    if (chunks.size() > 1) {
      prepared.blocks.reserve(chunks.size());
      for (const auto& chunk : chunks) {
        PreparedBlock block;
        block.payload = chunk;
        block.fallback_payload = chunk;
        block.payload_checksum = checksum_bytes(block.payload);
        block.fallback_checksum = block.payload_checksum;
        block.transforms = {TransformKind::ChunkReference};
        prepared.blocks.push_back(std::move(block));
      }
      return prepared;
    }
  }

  PreparedBlock block;
  block.payload.assign(input.begin(), input.end());
  block.fallback_payload.assign(input.begin(), input.end());
  block.payload_checksum = checksum_bytes(block.payload);
  block.fallback_checksum = checksum_bytes(block.fallback_payload);
  block.prefer_stored_raw = prefer_raw;
  prepared.blocks.push_back(std::move(block));

  return prepared;
}

std::vector<std::byte> TransformPipeline::restore_file(const ManifestEntry& entry,
                                                       const std::vector<DecodedBlock>& blocks) const {
  std::vector<std::byte> output;

  for (const auto& decoded : blocks) {
    std::vector<std::byte> payload = decoded.payload;

    for (auto iterator = decoded.transforms.rbegin();
         iterator != decoded.transforms.rend(); ++iterator) {
      switch (*iterator) {
        case TransformKind::None:
        case TransformKind::ChunkReference:
          break;
        case TransformKind::StreamNormalization:
          payload = transforms::reverse_stream_normalization(payload, decoded.recipe);
          break;
        case TransformKind::CodeDictionary:
          payload = transforms::reverse_code_dictionary(payload, decoded.recipe);
          break;
        case TransformKind::BcjX86:
          payload = transforms::reverse_bcj_x86(payload);
          break;
        case TransformKind::BcjArm64:
          payload = transforms::reverse_bcj_arm64(payload);
          break;
        case TransformKind::BcjArm:
          payload = transforms::reverse_bcj_arm(payload);
          break;
        case TransformKind::BcjArmThumb:
          payload = transforms::reverse_bcj_arm_thumb(payload);
          break;
        case TransformKind::DeltaFilter:
          payload = transforms::reverse_delta_filter(payload);
          break;
        case TransformKind::PngIdatStrip:
          payload = transforms::reverse_png_idat_strip(payload, decoded.recipe);
          break;
        case TransformKind::JpegBrunsli:
          payload = transforms::reverse_jpeg_brunsli(payload, 0);
          break;
        case TransformKind::PreflateDeflate:
          payload = transforms::reverse_png_preflate(payload, decoded.recipe);
          break;
      }
    }

    output.insert(output.end(), payload.begin(), payload.end());
  }

  if (checksum_bytes(output) != entry.content_checksum) {
    throw std::runtime_error("Transform pipeline failed to restore " + entry.archive_path);
  }

  return output;
}

}  // namespace devzip
