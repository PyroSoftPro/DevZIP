#include "devzip/solid_packer.h"

#include "devzip/archive_format.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace devzip {
namespace {

constexpr std::uint64_t kMaxCompressibleSolidPayloadBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxExecutableSolidPayloadBytes = 128ull * 1024ull * 1024ull;

enum class SolidBucket {
  None,
  Compressible,
  Executable,
};

std::vector<TransformKind> fallback_transforms_for(std::span<const TransformKind> transforms) {
  std::vector<TransformKind> kept;
  kept.reserve(transforms.size());
  for (const auto transform : transforms) {
    if (transform == TransformKind::ChunkReference) {
      kept.push_back(transform);
    }
  }
  return kept;
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

bool is_text_like_extension(std::string_view archive_path) {
  const auto path = std::filesystem::path(std::string(archive_path));
  const auto extension = lower_ascii(path.extension().string());
  static const std::array<std::string_view, 26> kTextExtensions = {
      ".c",      ".cc",    ".cpp",   ".cs",    ".css",   ".go",    ".h",
      ".hpp",    ".html",  ".ini",   ".java",  ".js",    ".json",  ".jsx",
      ".kt",     ".md",    ".py",    ".rb",    ".rs",    ".scss",  ".sql",
      ".toml",   ".ts",    ".tsx",   ".txt",   ".xml",
  };
  return std::find(kTextExtensions.begin(), kTextExtensions.end(), extension) != kTextExtensions.end();
}

bool is_executable_extension(std::string_view archive_path) {
  const auto extension = lower_ascii(
      std::filesystem::path(std::string(archive_path)).extension().string());
  return extension == ".exe" || extension == ".dll" ||
         extension == ".sys" || extension == ".ocx";
}

SolidBucket solid_bucket_for(const SolidPackItem& item) {
  if (item.prefer_stored_raw) {
    return SolidBucket::None;
  }
  if (is_executable_extension(item.archive_path)) {
    return SolidBucket::Executable;
  }
  return SolidBucket::Compressible;
}

std::uint64_t max_group_size_for(SolidBucket bucket) {
  switch (bucket) {
    case SolidBucket::Compressible:
      return kMaxCompressibleSolidPayloadBytes;
    case SolidBucket::Executable:
      return kMaxExecutableSolidPayloadBytes;
    case SolidBucket::None:
      return 0;
  }
  return 0;
}

void append_item(SolidPackGroup& group, const SolidPackItem& item) {
  SolidPackSpan span;
  span.entry_index = item.entry_index;
  span.block_index = item.block_index;
  span.payload_offset = static_cast<std::uint64_t>(group.payload.size());
  span.payload_size = static_cast<std::uint64_t>(item.block.payload.size());
  span.payload_checksum = item.block.payload_checksum;
  span.payload_transforms = item.block.transforms;
  span.payload_recipe = item.block.recipe;
  span.fallback_offset = static_cast<std::uint64_t>(group.fallback_payload.size());
  span.fallback_size = static_cast<std::uint64_t>(item.block.fallback_payload.size());
  span.fallback_checksum = item.block.fallback_checksum;
  span.fallback_transforms = fallback_transforms_for(item.block.transforms);

  group.payload.insert(group.payload.end(), item.block.payload.begin(), item.block.payload.end());
  group.fallback_payload.insert(group.fallback_payload.end(),
                                item.block.fallback_payload.begin(),
                                item.block.fallback_payload.end());
  group.spans.push_back(std::move(span));
  group.prefer_stored_raw = group.prefer_stored_raw || item.prefer_stored_raw;
}

SolidPackGroup single_item_group(const SolidPackItem& item) {
  SolidPackGroup group;
  append_item(group, item);
  return group;
}

}  // namespace

std::vector<SolidPackGroup> SolidPacker::pack(const std::vector<SolidPackItem>& items) const {
  std::vector<SolidPackGroup> groups;
  SolidPackGroup current_solid_group;
  SolidBucket current_bucket = SolidBucket::None;

  const auto flush_solid_group = [&]() {
    if (!current_solid_group.spans.empty()) {
      groups.push_back(std::move(current_solid_group));
      current_solid_group = SolidPackGroup{};
      current_bucket = SolidBucket::None;
    }
  };

  for (const auto& item : items) {
    const auto bucket = solid_bucket_for(item);
    const auto max_group_size = max_group_size_for(bucket);
    const bool solid_candidate =
        bucket != SolidBucket::None &&
        static_cast<std::uint64_t>(item.block.payload.size()) < max_group_size;

    if (!solid_candidate) {
      flush_solid_group();
      groups.push_back(single_item_group(item));
      continue;
    }

    if (current_bucket != SolidBucket::None && bucket != current_bucket) {
      flush_solid_group();
    }

    const auto next_size = static_cast<std::uint64_t>(current_solid_group.payload.size()) +
                           static_cast<std::uint64_t>(item.block.payload.size());
    if (!current_solid_group.spans.empty() && next_size > max_group_size) {
      flush_solid_group();
    }

    current_bucket = bucket;
    append_item(current_solid_group, item);
  }

  flush_solid_group();
  return groups;
}

}  // namespace devzip
