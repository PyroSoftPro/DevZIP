#include "devzip/extractor.h"

#include "devzip/source_scanner.h"
#include "devzip/solid_packer.h"
#include "devzip/temp_file_guard.h"
#include "devzip/transform_pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace devzip {
namespace {

constexpr std::uint64_t kMaxManifestBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxPayloadBytes = 2ull * 1024ull * 1024ull * 1024ull;

// Greedy nearest-neighbour similarity ordering for solid groups.
//
// Files are first partitioned by extension (so unrelated formats never mix),
// then within each extension run we reorder by content similarity: a normalised
// 256-bin byte histogram per block, chained from the largest block to its most
// similar neighbour.  Placing similar files adjacently lets LZMA2/ZPAQ exploit
// cross-file redundancy that a plain path sort would scatter.  This is the
// lightweight, dependency-free analogue of TLSH clustering.
struct BlockFingerprint {
  std::array<float, 256> hist{};
};

BlockFingerprint fingerprint_block(std::span<const std::byte> payload) {
  BlockFingerprint fp;
  std::array<std::uint64_t, 256> counts{};
  if (!payload.empty()) {
    // Sample up to ~64 KiB spread across the block to bound cost on big inputs.
    constexpr std::size_t kSampleBudget = 64u * 1024u;
    const std::size_t stride =
        payload.size() <= kSampleBudget ? 1 : payload.size() / kSampleBudget;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < payload.size(); i += stride) {
      ++counts[std::to_integer<unsigned char>(payload[i])];
      ++total;
    }
    if (total != 0) {
      double norm = 0.0;
      for (const auto c : counts) {
        const double v = static_cast<double>(c);
        norm += v * v;
      }
      norm = norm > 0.0 ? std::sqrt(norm) : 1.0;
      for (std::size_t b = 0; b < 256; ++b) {
        fp.hist[b] = static_cast<float>(static_cast<double>(counts[b]) / norm);
      }
    }
  }
  return fp;
}

float fingerprint_similarity(const BlockFingerprint& a, const BlockFingerprint& b) {
  float dot = 0.0f;
  for (std::size_t i = 0; i < 256; ++i) {
    dot += a.hist[i] * b.hist[i];
  }
  return dot;  // both vectors are L2-normalised => cosine similarity
}

// Reorders pack_items in place: stable extension partition, then greedy
// nearest-neighbour chaining within each extension run.  O(k^2) per run, so
// runs larger than kMaxSimilarityRun keep the cheap path order.
void order_items_by_similarity(std::vector<SolidPackItem>& items) {
  std::stable_sort(items.begin(), items.end(),
                   [](const SolidPackItem& a, const SolidPackItem& b) {
                     const auto ext_a = std::filesystem::path(a.archive_path).extension().string();
                     const auto ext_b = std::filesystem::path(b.archive_path).extension().string();
                     if (ext_a != ext_b) {
                       return ext_a < ext_b;
                     }
                     return a.archive_path < b.archive_path;
                   });

  constexpr std::size_t kMaxSimilarityRun = 4096;
  std::size_t run_begin = 0;
  while (run_begin < items.size()) {
    const auto ext = std::filesystem::path(items[run_begin].archive_path).extension().string();
    std::size_t run_end = run_begin + 1;
    while (run_end < items.size() &&
           std::filesystem::path(items[run_end].archive_path).extension().string() == ext) {
      ++run_end;
    }
    const std::size_t run_len = run_end - run_begin;
    if (run_len > 2 && run_len <= kMaxSimilarityRun) {
      std::vector<BlockFingerprint> fps(run_len);
      for (std::size_t i = 0; i < run_len; ++i) {
        fps[i] = fingerprint_block(items[run_begin + i].block.payload);
      }
      std::vector<char> used(run_len, 0);
      std::vector<std::size_t> order;
      order.reserve(run_len);
      std::size_t start = 0;
      for (std::size_t i = 1; i < run_len; ++i) {
        if (items[run_begin + i].block.payload.size() >
            items[run_begin + start].block.payload.size()) {
          start = i;
        }
      }
      used[start] = 1;
      order.push_back(start);
      std::size_t last = start;
      for (std::size_t placed = 1; placed < run_len; ++placed) {
        std::size_t best = run_len;
        float best_sim = -1.0f;
        for (std::size_t cand = 0; cand < run_len; ++cand) {
          if (used[cand]) {
            continue;
          }
          const float sim = fingerprint_similarity(fps[last], fps[cand]);
          if (sim > best_sim) {
            best_sim = sim;
            best = cand;
          }
        }
        used[best] = 1;
        order.push_back(best);
        last = best;
      }
      std::vector<SolidPackItem> reordered;
      reordered.reserve(run_len);
      for (const auto idx : order) {
        reordered.push_back(std::move(items[run_begin + idx]));
      }
      std::move(reordered.begin(), reordered.end(), items.begin() + run_begin);
    }
    run_begin = run_end;
  }
}

std::string to_hex(const std::array<std::uint8_t, 16>& checksum) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(checksum.size() * 2);
  for (const auto byte : checksum) {
    output.push_back(kHex[(byte >> 4) & 0x0F]);
    output.push_back(kHex[byte & 0x0F]);
  }
  return output;
}

void append_transform_signature(std::string& output, std::span<const TransformKind> transforms) {
  for (const auto transform : transforms) {
    output += std::to_string(static_cast<int>(transform));
    output.push_back(',');
  }
}

std::string prepared_block_dedup_key(const PreparedBlock& block) {
  std::string key;
  key.reserve(128 + block.recipe.size());
  key += to_hex(block.payload_checksum);
  key.push_back(':');
  key += std::to_string(block.payload.size());
  key.push_back(':');
  key += to_hex(block.fallback_checksum);
  key.push_back(':');
  key += std::to_string(block.fallback_payload.size());
  key.push_back(':');
  append_transform_signature(key, block.transforms);
  key.push_back(':');
  key += block.recipe;
  key.push_back(':');
  key.push_back(block.prefer_stored_raw ? '1' : '0');
  return key;
}

void write_u16(std::ostream& stream, std::uint16_t value) {
  char bytes[2] = {
      static_cast<char>(value & 0xFF),
      static_cast<char>((value >> 8) & 0xFF),
  };
  stream.write(bytes, sizeof(bytes));
}

void write_u64(std::ostream& stream, std::uint64_t value) {
  char bytes[8]{};
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<char>((value >> (index * 8)) & 0xFF);
  }
  stream.write(bytes, sizeof(bytes));
}

std::uint16_t read_u16(std::istream& stream) {
  char bytes[2]{};
  stream.read(bytes, sizeof(bytes));
  if (!stream) {
    throw std::runtime_error("Archive ended while reading a 16-bit value");
  }
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0])) |
         (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1])) << 8);
}

std::uint64_t read_u64(std::istream& stream) {
  char bytes[8]{};
  stream.read(bytes, sizeof(bytes));
  if (!stream) {
    throw std::runtime_error("Archive ended while reading a 64-bit value");
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index])) << (index * 8);
  }
  return value;
}

ArchiveHeader read_header(std::istream& stream) {
  ArchiveHeader header;
  stream.read(header.magic.data(), static_cast<std::streamsize>(header.magic.size()));
  if (!stream) {
    throw std::runtime_error("Archive ended before the header magic was fully read");
  }
  header.format_version = read_u16(stream);
  header.header_size = read_u16(stream);
  header.manifest_size = read_u64(stream);
  header.payload_size = read_u64(stream);
  stream.read(reinterpret_cast<char*>(header.manifest_checksum.data()),
              static_cast<std::streamsize>(header.manifest_checksum.size()));
  if (!stream) {
    throw std::runtime_error("Archive ended before the manifest checksum was fully read");
  }
  return header;
}

void write_header(std::ostream& stream, const ArchiveHeader& header) {
  stream.write(header.magic.data(), static_cast<std::streamsize>(header.magic.size()));
  write_u16(stream, header.format_version);
  write_u16(stream, header.header_size);
  write_u64(stream, header.manifest_size);
  write_u64(stream, header.payload_size);
  stream.write(reinterpret_cast<const char*>(header.manifest_checksum.data()),
               static_cast<std::streamsize>(header.manifest_checksum.size()));
}

std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Could not open file: " + path_to_utf8(path));
  }

  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) {
    throw std::runtime_error("Could not determine file length: " + path_to_utf8(path));
  }
  input.seekg(0, std::ios::beg);

  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
      throw std::runtime_error("File ended before all bytes could be read: " + path_to_utf8(path));
    }
  }
  return bytes;
}

std::filesystem::path source_path_for(const std::filesystem::path& source_root,
                                      const ManifestEntry& entry) {
  if (entry.archive_path == ".") {
    return source_root;
  }
  if (std::filesystem::is_regular_file(source_root)) {
    return source_root;
  }
  return source_root / std::filesystem::path(entry.archive_path);
}

std::filesystem::path destination_path_for(const std::filesystem::path& destination_root,
                                           const ManifestEntry& entry) {
  if (entry.archive_path == ".") {
    return destination_root;
  }

  const auto relative = std::filesystem::path(entry.archive_path);
  if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
    throw std::runtime_error("Archive entry escapes the destination root: " + entry.archive_path);
  }

  for (const auto& part : relative) {
    if (part == "..") {
      throw std::runtime_error("Archive entry escapes the destination root: " + entry.archive_path);
    }
  }

  return destination_root / relative;
}

struct ArchivePayload {
  ArchiveHeader header;
  ArchiveManifest manifest;
  std::vector<std::byte> payload;
};

std::vector<std::byte> compress_manifest_bytes(const std::vector<std::byte>& raw) {
  auto lzma2 = make_lzma2_backend();
  auto response = lzma2->compress(CompressionRequest{raw, "manifest"});
  std::vector<std::byte> result;
  result.reserve(8 + response.encoded.size());
  auto raw_size = static_cast<std::uint64_t>(raw.size());
  for (int i = 0; i < 8; ++i) {
    result.push_back(static_cast<std::byte>((raw_size >> (i * 8)) & 0xFF));
  }
  result.insert(result.end(), response.encoded.begin(), response.encoded.end());
  return result;
}

std::vector<std::byte> decompress_manifest_bytes(std::span<const std::byte> compressed) {
  if (compressed.size() < 9) {
    throw std::runtime_error("Compressed manifest too short");
  }
  std::uint64_t raw_size = 0;
  for (int i = 0; i < 8; ++i) {
    raw_size |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(compressed[i])) << (i * 8);
  }
  auto lzma2 = make_lzma2_backend();
  return lzma2->decompress(compressed.subspan(8), raw_size);
}

ArchivePayload load_archive(const std::filesystem::path& archive_path) {
  std::ifstream input(archive_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Could not open archive: " + path_to_utf8(archive_path));
  }

  const auto header = read_header(input);
  if (std::string(header.magic.data(), header.magic.size()) != "DVZ1") {
    throw std::runtime_error("Not a DVZ archive: " + path_to_utf8(archive_path));
  }
  if (header.format_version < 1 || header.format_version > kCurrentFormatVersion) {
    throw std::runtime_error("Unsupported DVZ version");
  }
  if (header.manifest_size > kMaxManifestBytes) {
    throw std::runtime_error("Manifest exceeds the supported in-memory limit");
  }
  if (header.payload_size > kMaxPayloadBytes) {
    throw std::runtime_error("Payload exceeds the supported in-memory limit");
  }

  std::vector<std::byte> stored_manifest(static_cast<std::size_t>(header.manifest_size));
  if (!stored_manifest.empty()) {
    input.read(reinterpret_cast<char*>(stored_manifest.data()),
               static_cast<std::streamsize>(stored_manifest.size()));
    if (input.gcount() != static_cast<std::streamsize>(stored_manifest.size())) {
      throw std::runtime_error("Archive ended before the manifest was fully read");
    }
  }

  std::vector<std::byte> manifest_bytes;
  std::uint16_t deserialize_version = header.format_version;
  if (header.format_version >= 3) {
    try {
      manifest_bytes = decompress_manifest_bytes(stored_manifest);
    } catch (const std::exception&) {
      throw std::runtime_error("Manifest checksum mismatch");
    }
    deserialize_version = 2;
  } else {
    manifest_bytes = std::move(stored_manifest);
  }

  if (checksum_bytes(manifest_bytes) != header.manifest_checksum) {
    throw std::runtime_error("Manifest checksum mismatch");
  }

  std::vector<std::byte> payload(static_cast<std::size_t>(header.payload_size));
  if (!payload.empty()) {
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
      throw std::runtime_error("Archive ended before the payload was fully read");
    }
  }

  return ArchivePayload{header, deserialize_manifest(manifest_bytes, deserialize_version), std::move(payload)};
}

bool archive_uses_backend_compression(const ArchiveManifest& manifest) {
  for (const auto& block : manifest.blocks) {
    if (block.storage == BlockStorageKind::BackendCompressed) {
      return true;
    }
  }
  return false;
}

void validate_backend_for_manifest(const ArchiveManifest& manifest, const CompressionBackend* backend) {
  if (!archive_uses_backend_compression(manifest)) {
    return;
  }
  if (backend == nullptr) {
    throw std::runtime_error("Archive requires a compression backend, but none was provided");
  }
  if (backend->stamp().name != manifest.backend.name) {
    throw std::runtime_error("Archive backend mismatch: expected " + manifest.backend.name + ", got " +
                             backend->stamp().name);
  }
}

std::vector<std::byte> slice_payload(const std::vector<std::byte>& payload, const BlockDescriptor& block) {
  if (block.payload_offset > static_cast<std::uint64_t>(payload.size())) {
    throw std::runtime_error("Archive payload offset exceeds payload size");
  }

  const auto start = static_cast<std::size_t>(block.payload_offset);
  const auto available = payload.size() - start;
  if (block.encoded_size > static_cast<std::uint64_t>(available)) {
    throw std::runtime_error("Archive payload truncated");
  }
  const auto end = start + static_cast<std::size_t>(block.encoded_size);
  return std::vector<std::byte>(payload.begin() + static_cast<std::ptrdiff_t>(start),
                                payload.begin() + static_cast<std::ptrdiff_t>(end));
}

std::vector<std::byte> decode_block_payload(const std::vector<std::byte>& payload,
                                            const BlockDescriptor& block,
                                            const CompressionBackend* backend) {
  const auto encoded = slice_payload(payload, block);
  if (block.storage == BlockStorageKind::StoredRaw) {
    return encoded;
  }
  if (backend == nullptr) {
    throw std::runtime_error("Archive references compressed payloads without a decoder");
  }
  return backend->decompress(encoded, block.raw_size);
}

const std::vector<std::byte>& decoded_block_bytes_for(
    const std::vector<std::byte>& payload,
    const BlockDescriptor& block,
    const CompressionBackend* backend,
    std::unordered_map<std::string, std::vector<std::byte>>& cache) {
  const auto cached = cache.find(block.id);
  if (cached != cache.end()) {
    return cached->second;
  }

  auto decoded = decode_block_payload(payload, block, backend);
  if (static_cast<std::uint64_t>(decoded.size()) != block.raw_size) {
    throw std::runtime_error("Decoded block size mismatch for " + block.id);
  }
  if (checksum_bytes(decoded) != block.checksum) {
    throw std::runtime_error("Decoded block checksum mismatch for " + block.id);
  }

  return cache.emplace(block.id, std::move(decoded)).first->second;
}

const std::vector<BlockSpanReference>& block_spans_for_entry(
    const ManifestEntry& entry,
    const std::unordered_map<std::string, BlockDescriptor>& blocks,
    std::vector<BlockSpanReference>& legacy_spans) {
  if (!entry.block_spans.empty()) {
    return entry.block_spans;
  }

  legacy_spans.clear();
  legacy_spans.reserve(entry.block_ids.size());
  for (const auto& block_id : entry.block_ids) {
    const auto iterator = blocks.find(block_id);
    if (iterator == blocks.end()) {
      throw std::runtime_error("Archive references a missing block: " + block_id);
    }

    BlockSpanReference span;
    span.block_id = block_id;
    span.offset = 0;
    span.size = iterator->second.raw_size;
    span.checksum = iterator->second.checksum;
    span.transforms = iterator->second.transforms;
    span.recipe = iterator->second.recipe;
    legacy_spans.push_back(std::move(span));
  }

  return legacy_spans;
}

std::vector<std::byte> slice_decoded_span(const std::vector<std::byte>& decoded,
                                          const BlockSpanReference& span) {
  if (span.offset > static_cast<std::uint64_t>(decoded.size())) {
    throw std::runtime_error("Block span offset exceeds decoded block size");
  }

  const auto start = static_cast<std::size_t>(span.offset);
  const auto available = decoded.size() - start;
  if (span.size > static_cast<std::uint64_t>(available)) {
    throw std::runtime_error("Block span exceeds decoded block size");
  }

  const auto end = start + static_cast<std::size_t>(span.size);
  return std::vector<std::byte>(decoded.begin() + static_cast<std::ptrdiff_t>(start),
                                decoded.begin() + static_cast<std::ptrdiff_t>(end));
}

void extract_archive_loaded(const ArchivePayload& archive,
                            const std::filesystem::path& destination_root,
                            CompressionBackend* backend) {
  TransformPipeline pipeline;
  std::unordered_map<std::string, BlockDescriptor> blocks;
  std::unordered_map<std::string, std::vector<std::byte>> decoded_block_cache;
  for (const auto& block : archive.manifest.blocks) {
    if (!blocks.emplace(block.id, block).second) {
      throw std::runtime_error("Archive contains duplicate block ids: " + block.id);
    }
  }

  validate_backend_for_manifest(archive.manifest, backend);
  std::filesystem::create_directories(destination_root);
  for (const auto& entry : archive.manifest.entries) {
    const auto destination = destination_path_for(destination_root, entry);
    if (entry.kind == EntryKind::Directory) {
      std::filesystem::create_directories(destination);
      continue;
    }

    if (entry.block_ids.empty() && entry.block_spans.empty()) {
      throw std::runtime_error("File entry has no block references: " + entry.archive_path);
    }

    std::filesystem::create_directories(destination.parent_path());
    std::vector<DecodedBlock> decoded_blocks;
    std::vector<BlockSpanReference> legacy_spans;
    for (const auto& span : block_spans_for_entry(entry, blocks, legacy_spans)) {
      const auto iterator = blocks.find(span.block_id);
      if (iterator == blocks.end()) {
        throw std::runtime_error("Archive references a missing block: " + span.block_id);
      }

      const auto& decoded = decoded_block_bytes_for(archive.payload, iterator->second, backend, decoded_block_cache);
      auto sliced = slice_decoded_span(decoded, span);
      if (checksum_bytes(sliced) != span.checksum) {
        throw std::runtime_error("Decoded block span checksum mismatch for " + span.block_id);
      }
      decoded_blocks.push_back(DecodedBlock{std::move(sliced), span.transforms, span.recipe});
    }
    const auto reconstructed = pipeline.restore_file(entry, decoded_blocks);
    TempFileGuard temp_file(destination);
    std::ofstream output(temp_file.temp_path(), std::ios::binary);
    if (!output) {
      throw std::runtime_error("Could not create output file: " + path_to_utf8(destination));
    }
    if (!reconstructed.empty()) {
      output.write(reinterpret_cast<const char*>(reconstructed.data()),
                   static_cast<std::streamsize>(reconstructed.size()));
    }
    output.close();
    if (!output) {
      throw std::runtime_error("Failed to finalize extracted file before commit");
    }
    temp_file.commit();
  }
}

void verify_archive_loaded(const ArchivePayload& archive, CompressionBackend* backend) {
  TransformPipeline pipeline;
  std::unordered_map<std::string, BlockDescriptor> blocks;
  std::unordered_map<std::string, std::vector<std::byte>> decoded_block_cache;
  for (const auto& block : archive.manifest.blocks) {
    if (!blocks.emplace(block.id, block).second) {
      throw std::runtime_error("Archive contains duplicate block ids: " + block.id);
    }
  }

  validate_backend_for_manifest(archive.manifest, backend);
  for (const auto& entry : archive.manifest.entries) {
    if (entry.kind != EntryKind::File) {
      continue;
    }

    if (entry.block_ids.empty() && entry.block_spans.empty()) {
      throw std::runtime_error("File entry has no block references: " + entry.archive_path);
    }

    std::vector<DecodedBlock> decoded_blocks;
    std::vector<BlockSpanReference> legacy_spans;
    for (const auto& span : block_spans_for_entry(entry, blocks, legacy_spans)) {
      const auto iterator = blocks.find(span.block_id);
      if (iterator == blocks.end()) {
        throw std::runtime_error("Archive references a missing block: " + span.block_id);
      }

      const auto& decoded = decoded_block_bytes_for(archive.payload, iterator->second, backend, decoded_block_cache);
      auto sliced = slice_decoded_span(decoded, span);
      if (checksum_bytes(sliced) != span.checksum) {
        throw std::runtime_error("Decoded block span checksum mismatch for " + span.block_id);
      }
      decoded_blocks.push_back(DecodedBlock{std::move(sliced), span.transforms, span.recipe});
    }
    (void)pipeline.restore_file(entry, decoded_blocks);
  }
}

}  // namespace

ArchiveWriteResult create_archive(const std::filesystem::path& source_path,
                                  const std::filesystem::path& archive_path,
                                  CompressionBackend& backend,
                                  const CompressionOptions& options) {
  auto scan = scan_source_tree(source_path);
  auto manifest = scan.manifest;
  manifest.backend = backend.stamp();
  TransformPipeline pipeline(options);
  SolidPacker solid_packer;

  std::vector<std::vector<std::byte>> encoded_payloads;
  std::uint64_t payload_offset = 0;
  std::unordered_map<std::string, BlockDescriptor> emitted_blocks;
  std::vector<SolidPackItem> pack_items;
  std::vector<std::vector<BlockSpanReference>> pending_block_spans(manifest.entries.size());

  // Deduplication: map a stable key derived from the prepared-file's block checksums
  // and transforms to the first entry index that produced it.  Subsequent files with
  // the same key are recorded as copies and will share block spans after compression.
  std::unordered_map<std::string, std::size_t> dedup_first_entry;
  std::vector<std::pair<std::size_t, std::size_t>> dedup_copies; // (later_idx, first_idx)
  std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> dedup_first_block;
  struct DeferredBlockCopy {
    std::size_t later_entry_index = 0;
    std::size_t later_block_index = 0;
    std::size_t first_entry_index = 0;
    std::size_t first_block_index = 0;
  };
  std::vector<DeferredBlockCopy> dedup_block_copies;

  for (std::size_t entry_index = 0; entry_index < manifest.entries.size(); ++entry_index) {
    auto& entry = manifest.entries[entry_index];
    if (entry.kind != EntryKind::File) {
      continue;
    }

    const auto path = source_path_for(source_path, entry);
    auto raw_bytes = read_file_bytes(path);
    auto prepared = pipeline.prepare_file(raw_bytes, entry.archive_path);
    entry.content_checksum = prepared.content_checksum;
    entry.block_ids.clear();
    entry.block_spans.clear();
    pending_block_spans[entry_index].resize(prepared.blocks.size());

    // Build a dedup key from the prepared-block payload checksums and transforms.
    // Two files that produce identical keys will decompress to identical bytes, so
    // the later file can safely reference the first file's stored blocks.
    std::string dedup_key = to_hex(prepared.content_checksum);
    for (const auto& blk : prepared.blocks) {
      dedup_key += ':';
      dedup_key += to_hex(blk.payload_checksum);
      dedup_key += ':';
      for (const auto t : blk.transforms) {
        dedup_key += std::to_string(static_cast<int>(t));
        dedup_key += ',';
      }
    }

    const auto dedup_it = dedup_first_entry.find(dedup_key);
    if (dedup_it != dedup_first_entry.end()) {
      dedup_copies.emplace_back(entry_index, dedup_it->second);
      continue;
    }
    dedup_first_entry.emplace(dedup_key, entry_index);

    for (std::size_t block_index = 0; block_index < prepared.blocks.size(); ++block_index) {
      auto& prepared_block = prepared.blocks[block_index];
      const auto block_dedup_key = prepared_block_dedup_key(prepared_block);
      const auto block_dedup_it = dedup_first_block.find(block_dedup_key);
      if (block_dedup_it != dedup_first_block.end()) {
        dedup_block_copies.push_back(DeferredBlockCopy{
            entry_index,
            block_index,
            block_dedup_it->second.first,
            block_dedup_it->second.second,
        });
        continue;
      }
      dedup_first_block.emplace(block_dedup_key, std::pair{entry_index, block_index});

      const bool prefer_stored_raw = prepared_block.prefer_stored_raw;
      pack_items.push_back(SolidPackItem{
          entry_index,
          block_index,
          entry.archive_path,
          std::move(prepared_block),
          prefer_stored_raw,
      });
    }
  }

  // Order items so similar content lands adjacently in solid groups, giving
  // LZMA2/ZPAQ a larger homogeneous context to exploit cross-file redundancy.
  order_items_by_similarity(pack_items);

  // Parallel compression: each solid group is independent, so we launch them
  // concurrently.  Each task gets its own backend instance (created from the
  // archive-level stamp) so that inner LZMA2/ZPAQ state is never shared.
  //
  // The sequential post-phase assigns payload_offset in the original group
  // order so the archive layout remains deterministic.
  struct CompressedGroupResult {
    std::vector<std::byte> data;                    // bytes to write into the payload stream
    std::uint64_t raw_size;                         // uncompressed size (for BlockDescriptor)
    bool stored_raw;
    std::array<std::uint8_t, 16> block_checksum;   // checksum of stored_plaintext
    struct PartialSpan {
      std::size_t entry_index;
      std::size_t block_index;
      BlockSpanReference ref;                       // block_id is filled in the sequential phase
    };
    std::vector<PartialSpan> partial_spans;
  };

  const BackendStamp backend_stamp = backend.stamp();
  const unsigned concurrency = std::max(1u, std::thread::hardware_concurrency());
  const bool backend_is_cloneable = [&]() {
    try {
      return static_cast<bool>(make_backend(backend_stamp));
    } catch (const std::exception&) {
      return false;
    }
  }();

  auto groups = solid_packer.pack(pack_items);
  const bool verify_roundtrip = options.verify_roundtrip;
  auto compress_group =
      [verify_roundtrip](SolidPackGroup group,
         std::string content_name,
         CompressionBackend& group_backend) -> CompressedGroupResult {
    CompressionResponse compressed;
    if (!group.prefer_stored_raw) {
      compressed = group_backend.compress(CompressionRequest{group.payload, content_name});
      if (verify_roundtrip && !compressed.encoded.empty()) {
        const auto check =
            group_backend.decompress(compressed.encoded, group.payload.size());
        if (check.size() != group.payload.size() ||
            std::memcmp(check.data(), group.payload.data(), check.size()) != 0) {
          throw std::runtime_error(
              "Create-time roundtrip verification failed for block in " + content_name);
        }
      }
    }
    const bool store_raw =
        group.prefer_stored_raw ||
        compressed.encoded.size() >= group.fallback_payload.size();
    const auto& stored_plaintext = store_raw ? group.fallback_payload : group.payload;
    const auto block_checksum = checksum_bytes(stored_plaintext);

    CompressedGroupResult result;
    result.raw_size = static_cast<std::uint64_t>(stored_plaintext.size());
    result.stored_raw = store_raw;
    result.block_checksum = block_checksum;
    result.data = store_raw ? std::move(group.fallback_payload)
                            : std::move(compressed.encoded);

    for (const auto& span : group.spans) {
      BlockSpanReference ref;
      // block_id is set in the sequential phase once we have the checksum
      if (store_raw) {
        ref.offset = span.fallback_offset;
        ref.size = span.fallback_size;
        ref.checksum = span.fallback_checksum;
        ref.transforms = span.fallback_transforms;
      } else {
        ref.offset = span.payload_offset;
        ref.size = span.payload_size;
        ref.checksum = span.payload_checksum;
        ref.transforms = span.payload_transforms;
        ref.recipe = span.payload_recipe;
      }
      result.partial_spans.push_back({span.entry_index, span.block_index, std::move(ref)});
    }
    return result;
  };

  std::vector<CompressedGroupResult> group_results;
  group_results.reserve(groups.size());

  if (!backend_is_cloneable || concurrency <= 1 || groups.size() <= 1) {
    for (auto& group : groups) {
      const std::string content_name =
          group.spans.empty() ? manifest.source_name
                              : manifest.entries[group.spans.front().entry_index].archive_path;
      group_results.push_back(compress_group(std::move(group), content_name, backend));
    }
  } else {
    std::vector<std::future<CompressedGroupResult>> futures;
    futures.reserve(groups.size());
    for (auto& group : groups) {
      const std::string content_name =
          group.spans.empty() ? manifest.source_name
                              : manifest.entries[group.spans.front().entry_index].archive_path;
      futures.push_back(std::async(
          std::launch::async,
          [g = std::move(group), backend_stamp, content_name, compress_group]() mutable {
            auto local_backend = make_backend(backend_stamp);
            return compress_group(std::move(g), std::move(content_name), *local_backend);
          }));
    }
    for (auto& future : futures) {
      group_results.push_back(future.get());
    }
  }

  // Sequential post-phase: assign payload_offset in original group order.
  for (auto& result : group_results) {
    const auto block_id = to_hex(result.block_checksum);

    for (auto& ps : result.partial_spans) {
      ps.ref.block_id = block_id;
      pending_block_spans[ps.entry_index][ps.block_index] = std::move(ps.ref);
    }

    if (emitted_blocks.contains(block_id)) {
      continue;
    }

    BlockDescriptor block;
    block.id = block_id;
    block.payload_offset = payload_offset;
    block.encoded_size = static_cast<std::uint64_t>(result.data.size());
    block.raw_size = result.raw_size;
    block.storage =
        result.stored_raw ? BlockStorageKind::StoredRaw : BlockStorageKind::BackendCompressed;
    block.checksum = result.block_checksum;

    payload_offset += block.encoded_size;
    emitted_blocks.emplace(block_id, block);
    manifest.blocks.push_back(block);
    encoded_payloads.push_back(std::move(result.data));
  }

  // Propagate block spans for identical prepared blocks after all canonical
  // spans have been materialized.
  for (const auto& copy : dedup_block_copies) {
    pending_block_spans[copy.later_entry_index][copy.later_block_index] =
        pending_block_spans[copy.first_entry_index][copy.first_block_index];
  }

  // Propagate block spans from originals to their content-identical copies.
  // This must happen after the second loop so that all original block_spans are set.
  for (const auto& [later_idx, first_idx] : dedup_copies) {
    pending_block_spans[later_idx] = pending_block_spans[first_idx];
  }

  for (std::size_t entry_index = 0; entry_index < manifest.entries.size(); ++entry_index) {
    auto& entry = manifest.entries[entry_index];
    if (entry.kind != EntryKind::File) {
      continue;
    }
    for (const auto& span : pending_block_spans[entry_index]) {
      if (span.block_id.empty()) {
        throw std::runtime_error("Archive entry is missing a resolved block span: " +
                                 entry.archive_path);
      }
    }
    entry.block_spans = pending_block_spans[entry_index];
  }

  const auto manifest_bytes = serialize_manifest(manifest, kCurrentFormatVersion);
  const auto compressed_manifest = compress_manifest_bytes(manifest_bytes);

  ArchiveHeader header;
  header.format_version = kCurrentFormatVersion;
  header.manifest_size = compressed_manifest.size();
  header.payload_size = payload_offset;
  header.manifest_checksum = checksum_bytes(manifest_bytes);

  TempFileGuard temp_file(archive_path);
  std::ofstream output(temp_file.temp_path(), std::ios::binary);
  if (!output) {
    throw std::runtime_error("Could not open temporary archive: " + path_to_utf8(temp_file.temp_path()));
  }

  write_header(output, header);
  output.write(reinterpret_cast<const char*>(compressed_manifest.data()),
               static_cast<std::streamsize>(compressed_manifest.size()));
  for (const auto& payload : encoded_payloads) {
    if (!payload.empty()) {
      output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
  }
  output.close();
  if (!output) {
    throw std::runtime_error("Failed to finalize archive output before commit");
  }

  temp_file.commit();
  return ArchiveWriteResult{archive_path, manifest};
}

ArchiveManifest read_archive_manifest(const std::filesystem::path& archive_path) {
  return load_archive(archive_path).manifest;
}

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination_root,
                     CompressionBackend& backend) {
  const auto archive = load_archive(archive_path);
  extract_archive_loaded(archive, destination_root, &backend);
}

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& destination_root) {
  const auto archive = load_archive(archive_path);
  std::unique_ptr<CompressionBackend> backend;
  if (archive_uses_backend_compression(archive.manifest)) {
    backend = make_backend(archive.manifest.backend);
  }
  extract_archive_loaded(archive, destination_root, backend.get());
}

void verify_archive(const std::filesystem::path& archive_path, CompressionBackend& backend) {
  const auto archive = load_archive(archive_path);
  verify_archive_loaded(archive, &backend);
}

void verify_archive(const std::filesystem::path& archive_path) {
  const auto archive = load_archive(archive_path);
  std::unique_ptr<CompressionBackend> backend;
  if (archive_uses_backend_compression(archive.manifest)) {
    backend = make_backend(archive.manifest.backend);
  }
  verify_archive_loaded(archive, backend.get());
}

}  // namespace devzip
