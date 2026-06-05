#include "devzip/archive_format.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

namespace devzip {
namespace {

class BinaryWriter {
 public:
  void write_u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }

  void write_u16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
      write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void write_u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void write_u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void write_bool(bool value) { write_u8(value ? 1 : 0); }

  void write_string(const std::string& value) {
    write_u32(static_cast<std::uint32_t>(value.size()));
    for (unsigned char byte : value) {
      write_u8(byte);
    }
  }

  void write_checksum(const std::array<std::uint8_t, 16>& value) {
    for (std::uint8_t byte : value) {
      write_u8(byte);
    }
  }

  const std::vector<std::byte>& data() const { return data_; }

 private:
  std::vector<std::byte> data_;
};

class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  std::uint8_t read_u8() {
    ensure_available(1);
    return static_cast<std::uint8_t>(bytes_[cursor_++]);
  }

  std::uint16_t read_u16() {
    std::uint16_t value = 0;
    for (int shift = 0; shift < 16; shift += 8) {
      value |= static_cast<std::uint16_t>(read_u8()) << shift;
    }
    return value;
  }

  std::uint32_t read_u32() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(read_u8()) << shift;
    }
    return value;
  }

  std::uint64_t read_u64() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(read_u8()) << shift;
    }
    return value;
  }

  bool read_bool() { return read_u8() != 0; }

  std::string read_string() {
    const auto length = read_u32();
    ensure_available(length);
    std::string value;
    value.reserve(length);
    for (std::uint32_t index = 0; index < length; ++index) {
      value.push_back(static_cast<char>(read_u8()));
    }
    return value;
  }

  std::array<std::uint8_t, 16> read_checksum() {
    std::array<std::uint8_t, 16> output{};
    for (std::uint8_t& byte : output) {
      byte = read_u8();
    }
    return output;
  }

 private:
  void ensure_available(std::size_t requested) const {
    if (cursor_ > bytes_.size() || requested > bytes_.size() - cursor_) {
      throw std::runtime_error("Manifest data ended unexpectedly");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t cursor_ = 0;
};

std::uint64_t fnv1a_lane(std::span<const std::byte> bytes, std::uint64_t seed) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t value = seed;
  for (std::byte byte : bytes) {
    value ^= static_cast<std::uint8_t>(byte);
    value *= kPrime;
  }
  return value;
}

std::string narrow_u8(std::u8string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char8_t byte : value) {
    output.push_back(static_cast<char>(byte));
  }
  return output;
}

std::string archive_stem_for(const std::filesystem::path& source_path) {
  const auto filename = source_path.filename();
  if (!filename.empty()) {
    return path_to_utf8(filename);
  }
  return "archive";
}

EntryKind checked_entry_kind(std::uint8_t raw) {
  switch (raw) {
    case static_cast<std::uint8_t>(EntryKind::File):
      return EntryKind::File;
    case static_cast<std::uint8_t>(EntryKind::Directory):
      return EntryKind::Directory;
    default:
      throw std::runtime_error("Manifest contains an unknown entry kind");
  }
}

TransformKind checked_transform_kind(std::uint8_t raw) {
  switch (raw) {
    case static_cast<std::uint8_t>(TransformKind::None):
      return TransformKind::None;
    case static_cast<std::uint8_t>(TransformKind::ChunkReference):
      return TransformKind::ChunkReference;
    case static_cast<std::uint8_t>(TransformKind::StreamNormalization):
      return TransformKind::StreamNormalization;
    case static_cast<std::uint8_t>(TransformKind::CodeDictionary):
      return TransformKind::CodeDictionary;
    case static_cast<std::uint8_t>(TransformKind::BcjX86):
      return TransformKind::BcjX86;
    case static_cast<std::uint8_t>(TransformKind::DeltaFilter):
      return TransformKind::DeltaFilter;
    case static_cast<std::uint8_t>(TransformKind::PngIdatStrip):
      return TransformKind::PngIdatStrip;
    case static_cast<std::uint8_t>(TransformKind::JpegBrunsli):
      return TransformKind::JpegBrunsli;
    case static_cast<std::uint8_t>(TransformKind::PreflateDeflate):
      return TransformKind::PreflateDeflate;
    case static_cast<std::uint8_t>(TransformKind::BcjArm64):
      return TransformKind::BcjArm64;
    case static_cast<std::uint8_t>(TransformKind::BcjArm):
      return TransformKind::BcjArm;
    case static_cast<std::uint8_t>(TransformKind::BcjArmThumb):
      return TransformKind::BcjArmThumb;
    default:
      throw std::runtime_error("Manifest contains an unknown transform kind");
  }
}

BlockStorageKind checked_block_storage_kind(std::uint8_t raw) {
  switch (raw) {
    case static_cast<std::uint8_t>(BlockStorageKind::BackendCompressed):
      return BlockStorageKind::BackendCompressed;
    case static_cast<std::uint8_t>(BlockStorageKind::StoredRaw):
      return BlockStorageKind::StoredRaw;
    default:
      throw std::runtime_error("Manifest contains an unknown block storage kind");
  }
}

}  // namespace

std::string path_to_utf8(const std::filesystem::path& path) {
  return narrow_u8(path.u8string());
}

std::string path_to_generic_utf8(const std::filesystem::path& path) {
  return narrow_u8(path.generic_u8string());
}

std::vector<std::byte> serialize_manifest(const ArchiveManifest& manifest, std::uint16_t format_version) {
  if (format_version < 1 || format_version > kCurrentFormatVersion) {
    throw std::runtime_error("Unsupported manifest format version");
  }

  BinaryWriter writer;

  writer.write_string(manifest.backend.name);
  writer.write_string(manifest.backend.version);
  writer.write_string(manifest.source_name);
  writer.write_string(manifest.created_utc);
  writer.write_bool(manifest.deterministic);

  writer.write_u32(static_cast<std::uint32_t>(manifest.entries.size()));
  for (const auto& entry : manifest.entries) {
    writer.write_string(entry.archive_path);
    writer.write_u8(static_cast<std::uint8_t>(entry.kind));
    writer.write_u64(entry.size);
    writer.write_u64(entry.modified_time_ns);
    writer.write_u32(entry.windows_attributes);
    writer.write_checksum(entry.content_checksum);
    writer.write_u32(static_cast<std::uint32_t>(entry.block_ids.size()));
    for (const auto& block_id : entry.block_ids) {
      writer.write_string(block_id);
    }
    if (format_version >= 2) {
      writer.write_u32(static_cast<std::uint32_t>(entry.block_spans.size()));
      for (const auto& span : entry.block_spans) {
        writer.write_string(span.block_id);
        writer.write_u64(span.offset);
        writer.write_u64(span.size);
        writer.write_checksum(span.checksum);
        writer.write_u32(static_cast<std::uint32_t>(span.transforms.size()));
        for (const auto transform : span.transforms) {
          writer.write_u8(static_cast<std::uint8_t>(transform));
        }
        writer.write_string(span.recipe);
      }
    }
  }

  writer.write_u32(static_cast<std::uint32_t>(manifest.blocks.size()));
  for (const auto& block : manifest.blocks) {
    writer.write_string(block.id);
    writer.write_u64(block.payload_offset);
    writer.write_u64(block.encoded_size);
    writer.write_u64(block.raw_size);
    writer.write_u8(static_cast<std::uint8_t>(block.storage));
    writer.write_checksum(block.checksum);
    writer.write_u32(static_cast<std::uint32_t>(block.transforms.size()));
    for (const auto transform : block.transforms) {
      writer.write_u8(static_cast<std::uint8_t>(transform));
    }
    writer.write_string(block.recipe);
  }

  return writer.data();
}

ArchiveManifest deserialize_manifest(std::span<const std::byte> bytes, std::uint16_t format_version) {
  if (format_version < 1 || format_version > kCurrentFormatVersion) {
    throw std::runtime_error("Unsupported manifest format version");
  }

  BinaryReader reader(bytes);
  ArchiveManifest manifest;

  manifest.backend.name = reader.read_string();
  manifest.backend.version = reader.read_string();
  manifest.source_name = reader.read_string();
  manifest.created_utc = reader.read_string();
  manifest.deterministic = reader.read_bool();

  const auto entry_count = reader.read_u32();
  manifest.entries.reserve(entry_count);
  for (std::uint32_t index = 0; index < entry_count; ++index) {
    ManifestEntry entry;
    entry.archive_path = reader.read_string();
    entry.kind = checked_entry_kind(reader.read_u8());
    entry.size = reader.read_u64();
    entry.modified_time_ns = reader.read_u64();
    entry.windows_attributes = reader.read_u32();
    entry.content_checksum = reader.read_checksum();
    const auto block_count = reader.read_u32();
    entry.block_ids.reserve(block_count);
    for (std::uint32_t block_index = 0; block_index < block_count; ++block_index) {
      entry.block_ids.push_back(reader.read_string());
    }
    if (format_version >= 2) {
      const auto span_count = reader.read_u32();
      entry.block_spans.reserve(span_count);
      for (std::uint32_t span_index = 0; span_index < span_count; ++span_index) {
        BlockSpanReference span;
        span.block_id = reader.read_string();
        span.offset = reader.read_u64();
        span.size = reader.read_u64();
        span.checksum = reader.read_checksum();
        const auto transform_count = reader.read_u32();
        span.transforms.reserve(transform_count);
        for (std::uint32_t transform_index = 0; transform_index < transform_count; ++transform_index) {
          span.transforms.push_back(checked_transform_kind(reader.read_u8()));
        }
        span.recipe = reader.read_string();
        entry.block_spans.push_back(std::move(span));
      }
    }
    manifest.entries.push_back(std::move(entry));
  }

  const auto block_count = reader.read_u32();
  manifest.blocks.reserve(block_count);
  for (std::uint32_t index = 0; index < block_count; ++index) {
    BlockDescriptor block;
    block.id = reader.read_string();
    block.payload_offset = reader.read_u64();
    block.encoded_size = reader.read_u64();
    block.raw_size = reader.read_u64();
    block.storage = checked_block_storage_kind(reader.read_u8());
    block.checksum = reader.read_checksum();
    const auto transform_count = reader.read_u32();
    block.transforms.reserve(transform_count);
    for (std::uint32_t transform_index = 0; transform_index < transform_count; ++transform_index) {
      block.transforms.push_back(checked_transform_kind(reader.read_u8()));
    }
    block.recipe = reader.read_string();
    manifest.blocks.push_back(std::move(block));
  }

  return manifest;
}

std::filesystem::path default_archive_path(const std::filesystem::path& source_path,
                                           const std::filesystem::path& parent_directory) {
  std::string stem = archive_stem_for(source_path);
  std::filesystem::path candidate = parent_directory / (stem + ".dvz");
  if (!std::filesystem::exists(candidate)) {
    return candidate;
  }

  for (int suffix = 2; suffix < 1000; ++suffix) {
    candidate = parent_directory / (stem + " (" + std::to_string(suffix) + ").dvz");
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  throw std::runtime_error("Could not find an available archive name");
}

std::array<std::uint8_t, 16> checksum_bytes(std::span<const std::byte> bytes) {
  const std::uint64_t lane_a = fnv1a_lane(bytes, 14695981039346656037ull);
  const std::uint64_t lane_b = fnv1a_lane(bytes, 1099511628211ull * 97ull);

  std::array<std::uint8_t, 16> output{};
  for (int index = 0; index < 8; ++index) {
    output[index] = static_cast<std::uint8_t>((lane_a >> (index * 8)) & 0xFF);
    output[8 + index] = static_cast<std::uint8_t>((lane_b >> (index * 8)) & 0xFF);
  }
  return output;
}

std::array<std::uint8_t, 16> fast_hash_bytes(std::span<const std::byte> bytes) {
#if defined(__SSE4_2__) && (defined(__x86_64__) || defined(_M_X64))
  unsigned long long crc_a = 0x1F0D3804ull;
  unsigned long long crc_b = 0xA9BFE7C1ull;

  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const std::size_t len = bytes.size();
  std::size_t i = 0;

  for (; i + 8 <= len; i += 8) {
    std::uint64_t word;
    std::memcpy(&word, data + i, sizeof(word));
    crc_a = _mm_crc32_u64(crc_a, word);
    crc_b = _mm_crc32_u64(crc_b, word ^ 0x9E3779B97F4A7C15ULL);
  }
  for (; i < len; ++i) {
    crc_a = _mm_crc32_u8(static_cast<unsigned int>(crc_a), data[i]);
    crc_b = _mm_crc32_u8(static_cast<unsigned int>(crc_b), data[i]);
  }

  unsigned long long crc_c = crc_a ^ 0x5BD1E995ull;
  unsigned long long crc_d = crc_b ^ 0x1B873593ull;
  i = 0;
  for (; i + 8 <= len; i += 8) {
    std::uint64_t word;
    std::memcpy(&word, data + i, sizeof(word));
    crc_c = _mm_crc32_u64(crc_c, word ^ crc_d);
    crc_d = _mm_crc32_u64(crc_d, word ^ crc_c);
  }
  for (; i < len; ++i) {
    crc_c = _mm_crc32_u8(static_cast<unsigned int>(crc_c), data[i]);
    crc_d = _mm_crc32_u8(static_cast<unsigned int>(crc_d), data[i]);
  }

  const auto w0 = static_cast<std::uint32_t>(crc_a);
  const auto w1 = static_cast<std::uint32_t>(crc_b);
  const auto w2 = static_cast<std::uint32_t>(crc_c);
  const auto w3 = static_cast<std::uint32_t>(crc_d);

  std::array<std::uint8_t, 16> output{};
  std::memcpy(output.data() + 0, &w0, 4);
  std::memcpy(output.data() + 4, &w1, 4);
  std::memcpy(output.data() + 8, &w2, 4);
  std::memcpy(output.data() + 12, &w3, 4);
  return output;
#else
  return checksum_bytes(bytes);
#endif
}

}  // namespace devzip
