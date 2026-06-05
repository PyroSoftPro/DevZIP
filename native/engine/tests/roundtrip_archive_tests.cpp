#include "devzip/backend.h"
#include "devzip/extractor.h"
#include "test_harness.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
  auto root = std::filesystem::temp_directory_path() / ("devzip-archive-" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  input.seekg(0, std::ios::beg);

  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  return bytes;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

const devzip::ManifestEntry& find_entry(const devzip::ArchiveManifest& manifest,
                                        std::string_view archive_path) {
  for (const auto& entry : manifest.entries) {
    if (entry.archive_path == archive_path) {
      return entry;
    }
  }
  throw std::runtime_error("Missing manifest entry: " + std::string(archive_path));
}

class InflatingBackend final : public devzip::CompressionBackend {
 public:
  devzip::BackendStamp stamp() const override { return {"inflating", "1"}; }

  devzip::CompressionResponse compress(const devzip::CompressionRequest& request) override {
    devzip::CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = devzip::checksum_bytes(request.input);
    response.encoded.assign(request.input.begin(), request.input.end());
    response.encoded.insert(response.encoded.end(), request.input.begin(), request.input.end());
    response.encoded.push_back(std::byte{0});
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte>,
                                    std::uint64_t /*expected_raw_size*/) const override {
    throw std::runtime_error("Stored raw blocks should bypass backend decompression");
  }
};

}  // namespace

DEVZIP_TEST(libzpaq_backend_round_trips_a_directory_archive) {
  const auto root = make_temp_root("roundtrip");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source / "nested");
  write_text(source / "a.txt", "alpha");
  write_text(source / "nested" / "b.txt", "beta beta beta");

  auto backend = devzip::make_libzpaq_backend();
  devzip::create_archive(source, archive, *backend);
  devzip::verify_archive(archive, *backend);
  devzip::extract_archive(archive, extracted, *backend);

  DEVZIP_REQUIRE(read_text(extracted / "a.txt") == "alpha", "Extracted root file should match");
  DEVZIP_REQUIRE(read_text(extracted / "nested" / "b.txt") == "beta beta beta",
                 "Extracted nested file should match");
}

DEVZIP_TEST(lzma2_backend_round_trips_a_directory_archive) {
  const auto root = make_temp_root("roundtrip-lzma2");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source / "nested");
  write_text(source / "a.txt", "alpha");
  write_text(source / "nested" / "b.txt", "beta beta beta");

  auto backend = devzip::make_lzma2_backend();
  devzip::create_archive(source, archive, *backend);
  devzip::verify_archive(archive);
  devzip::extract_archive(archive, extracted);

  const auto manifest = devzip::read_archive_manifest(archive);
  DEVZIP_REQUIRE(manifest.backend.name == "lzma2", "Archive should record the LZMA2 backend");
  DEVZIP_REQUIRE(read_text(extracted / "a.txt") == "alpha", "Extracted root file should match");
  DEVZIP_REQUIRE(read_text(extracted / "nested" / "b.txt") == "beta beta beta",
                 "Extracted nested file should match");
}

DEVZIP_TEST(ppmd_backend_round_trips_a_directory_archive) {
  const auto root = make_temp_root("roundtrip-ppmd");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source / "nested");
  write_text(source / "a.txt", "alpha");
  write_text(source / "nested" / "b.txt", "beta beta beta");

  auto backend = devzip::make_ppmd_backend();
  devzip::create_archive(source, archive, *backend);
  devzip::verify_archive(archive);
  devzip::extract_archive(archive, extracted);

  const auto manifest = devzip::read_archive_manifest(archive);
  DEVZIP_REQUIRE(manifest.backend.name == "ppmd", "Archive should record the PPMd backend");
  DEVZIP_REQUIRE(read_text(extracted / "a.txt") == "alpha", "Extracted root file should match");
  DEVZIP_REQUIRE(read_text(extracted / "nested" / "b.txt") == "beta beta beta",
                 "Extracted nested file should match");
}

DEVZIP_TEST(dzcm_backend_round_trips_text_and_binary) {
  const auto root = make_temp_root("roundtrip-dzcm");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source / "nested");
  write_text(source / "a.txt", "alpha alpha alpha the quick brown fox jumps over the lazy dog");
  write_text(source / "nested" / "b.txt", "beta beta beta beta beta beta beta beta beta");

  // A blob spanning every byte value, repeated, to exercise the full coder and
  // the match model.
  std::vector<std::byte> blob;
  blob.reserve(64 * 1024);
  for (int repeat = 0; repeat < 256; ++repeat) {
    for (int value = 0; value < 256; ++value) {
      blob.push_back(static_cast<std::byte>((value * 7 + repeat) & 0xFF));
    }
  }
  write_bytes(source / "blob.bin", blob);

  auto backend = devzip::make_dzcm_backend();
  devzip::create_archive(source, archive, *backend);
  devzip::verify_archive(archive);
  devzip::extract_archive(archive, extracted);

  const auto manifest = devzip::read_archive_manifest(archive);
  DEVZIP_REQUIRE(manifest.backend.name == "dzcm", "Archive should record the DZCM backend");
  DEVZIP_REQUIRE(read_text(extracted / "a.txt") ==
                     "alpha alpha alpha the quick brown fox jumps over the lazy dog",
                 "DZCM extracted root file should match");
  DEVZIP_REQUIRE(read_text(extracted / "nested" / "b.txt") ==
                     "beta beta beta beta beta beta beta beta beta",
                 "DZCM extracted nested file should match");
  DEVZIP_REQUIRE(read_bytes(extracted / "blob.bin") == blob,
                 "DZCM should reproduce a full-byte-range binary blob exactly");
}

DEVZIP_TEST(auto_backend_reads_legacy_libzpaq_archive) {
  const auto root = make_temp_root("roundtrip-libzpaq-auto");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source / "nested");
  write_text(source / "a.txt", "alpha");
  write_text(source / "nested" / "b.txt", "beta beta beta");

  auto backend = devzip::make_libzpaq_backend();
  devzip::create_archive(source, archive, *backend);
  devzip::verify_archive(archive);
  devzip::extract_archive(archive, extracted);

  DEVZIP_REQUIRE(read_text(extracted / "a.txt") == "alpha", "Auto backend should extract legacy libzpaq files");
  DEVZIP_REQUIRE(read_text(extracted / "nested" / "b.txt") == "beta beta beta",
                 "Auto backend should extract nested legacy libzpaq files");
}

DEVZIP_TEST(archive_stores_raw_blocks_when_backend_expands_payload) {
  const auto root = make_temp_root("stored-raw");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);
  write_text(source / "a.txt", "repeat me repeat me repeat me repeat me repeat me");

  InflatingBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);

  DEVZIP_REQUIRE(manifest.blocks.size() == 1, "Expected one block for the single source file");
  DEVZIP_REQUIRE(manifest.blocks.front().storage == devzip::BlockStorageKind::StoredRaw,
                 "Expanding payloads should be stored raw");
  DEVZIP_REQUIRE(manifest.entries.size() >= 2, "Expected the manifest to include the root and file entry");
  DEVZIP_REQUIRE(manifest.entries.back().block_spans.size() == 1,
                 "Stored raw files should keep a single span reference");
  DEVZIP_REQUIRE(manifest.entries.back().block_spans.front().transforms.empty(),
                 "Stored raw file spans should drop reversible content transforms");

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_text(extracted / "a.txt") == "repeat me repeat me repeat me repeat me repeat me",
                 "Stored raw archives should extract the original content");
}

DEVZIP_TEST(chunked_archive_keeps_chunk_reference_when_stored_raw) {
  const auto root = make_temp_root("stored-raw-chunked");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);

  std::string payload;
  for (int index = 0; index < 12000; ++index) {
    payload += "repeat me repeat me repeat me repeat me repeat me\n";
  }
  write_text(source / "chunked.txt", payload);

  InflatingBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);

  DEVZIP_REQUIRE(!manifest.blocks.empty(), "Expected at least one stored block");
  DEVZIP_REQUIRE(manifest.entries.back().block_spans.size() > 1,
                 "Chunked files should keep multiple span references");
  for (const auto& block : manifest.blocks) {
    DEVZIP_REQUIRE(block.storage == devzip::BlockStorageKind::StoredRaw,
                   "All chunked solid blocks should be stored raw when compression expands them");
  }
  for (const auto& span : manifest.entries.back().block_spans) {
    DEVZIP_REQUIRE(span.transforms.size() == 1 &&
                       span.transforms.front() == devzip::TransformKind::ChunkReference,
                   "Stored raw chunked spans should keep only the chunk reference marker");
  }

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_text(extracted / "chunked.txt") == payload,
                 "Chunked stored raw archives should reassemble the original payload");
}

DEVZIP_TEST(chunked_near_duplicate_files_reuse_identical_spans) {
  const auto root = make_temp_root("chunked-near-duplicates");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);

  auto make_chunk = [](const std::string& label) {
    std::string chunk;
    while (chunk.size() < 96 * 1024) {
      chunk += label;
      chunk += " :: shared context line with enough repetition to trigger chunking\n";
    }
    return chunk;
  };

  const std::string shared_prefix = make_chunk("prefix");
  const std::string shared_suffix = make_chunk("suffix");
  const std::string unique_a = make_chunk("unique-a");
  const std::string unique_b = make_chunk("unique-b");

  const std::string alpha_payload = shared_prefix + unique_a + shared_suffix;
  const std::string beta_payload = shared_prefix + unique_b + shared_suffix;

  InflatingBackend backend;
  write_text(source / "alpha.txt", alpha_payload);
  write_text(source / "beta.txt", beta_payload);

  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);
  const auto& alpha = manifest.entries[1];
  const auto& beta = manifest.entries[2];

  DEVZIP_REQUIRE(alpha.block_spans.size() > 1 && beta.block_spans.size() > 1,
                 "Near-duplicate text fixtures should be chunked");

  std::size_t shared_span_count = 0;
  for (const auto& alpha_span : alpha.block_spans) {
    for (const auto& beta_span : beta.block_spans) {
      if (alpha_span.block_id == beta_span.block_id &&
          alpha_span.offset == beta_span.offset &&
          alpha_span.size == beta_span.size) {
        ++shared_span_count;
        break;
      }
    }
  }
  DEVZIP_REQUIRE(shared_span_count >= 2,
                 "Near-duplicate chunked files should reuse multiple shared spans");

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_text(extracted / "alpha.txt") == alpha_payload,
                 "Chunked near-duplicate extraction should preserve the first file");
  DEVZIP_REQUIRE(read_text(extracted / "beta.txt") == beta_payload,
                 "Chunked near-duplicate extraction should preserve the second file");
}

DEVZIP_TEST(chunked_files_round_trip_when_backend_compressed) {
  const auto root = make_temp_root("chunked-compressed");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);

  std::string payload;
  for (int index = 0; index < 32000; ++index) {
    payload += "chunk-compressed line with repeated vocabulary and structure\n";
  }
  write_text(source / "chunked.txt", payload);

  auto backend = devzip::make_lzma2_backend();
  devzip::create_archive(source, archive, *backend);
  const auto manifest = devzip::read_archive_manifest(archive);
  const auto& entry = find_entry(manifest, "chunked.txt");

  DEVZIP_REQUIRE(entry.block_spans.size() > 1,
                 "Large repetitive text should stay chunked on the compressed path");

  bool saw_backend_compressed = false;
  for (const auto& block : manifest.blocks) {
    if (block.storage == devzip::BlockStorageKind::BackendCompressed) {
      saw_backend_compressed = true;
      break;
    }
  }
  DEVZIP_REQUIRE(saw_backend_compressed,
                 "Chunked repetitive text should produce at least one backend-compressed block");

  devzip::verify_archive(archive);
  devzip::extract_archive(archive, extracted);

  DEVZIP_REQUIRE(read_text(extracted / "chunked.txt") == payload,
                 "Compressed chunked archive should extract the original payload");
}

DEVZIP_TEST(identical_chunked_files_share_the_same_span_list) {
  const auto root = make_temp_root("chunked-identical-dedup");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);

  std::string payload;
  for (int index = 0; index < 24000; ++index) {
    payload += "identical chunked file content with repeated lines and sections\n";
  }

  write_text(source / "alpha.txt", payload);
  write_text(source / "beta.txt", payload);

  InflatingBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);
  const auto& alpha = find_entry(manifest, "alpha.txt");
  const auto& beta = find_entry(manifest, "beta.txt");

  DEVZIP_REQUIRE(alpha.block_spans.size() > 1 && beta.block_spans.size() == alpha.block_spans.size(),
                 "Identical large text files should both be chunked");
  for (std::size_t index = 0; index < alpha.block_spans.size(); ++index) {
    DEVZIP_REQUIRE(alpha.block_spans[index].block_id == beta.block_spans[index].block_id &&
                       alpha.block_spans[index].offset == beta.block_spans[index].offset &&
                       alpha.block_spans[index].size == beta.block_spans[index].size,
                   "Entry-level dedup should reuse the exact same chunk span list");
  }

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_text(extracted / "alpha.txt") == payload,
                 "Identical chunked dedup should preserve the first file");
  DEVZIP_REQUIRE(read_text(extracted / "beta.txt") == payload,
                 "Identical chunked dedup should preserve the second file");
}

DEVZIP_TEST(text_like_files_share_a_solid_block) {
  const auto root = make_temp_root("solid-group");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);
  write_text(source / "alpha.txt", "shared line one\nshared line two\nshared line three\n");
  write_text(source / "beta.txt", "shared line one\nshared line two\nshared line three\n");

  InflatingBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);

  DEVZIP_REQUIRE(manifest.blocks.size() == 1, "Text-like files should share one solid block");
  const auto& alpha = manifest.entries[1];
  const auto& beta = manifest.entries[2];
  DEVZIP_REQUIRE(alpha.block_spans.size() == 1 && beta.block_spans.size() == 1,
                 "Each text file should map to one span within the shared solid block");
  DEVZIP_REQUIRE(alpha.block_spans.front().block_id == beta.block_spans.front().block_id,
                 "Solid-group members should reference the same block id");

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_text(extracted / "alpha.txt") == "shared line one\nshared line two\nshared line three\n",
                 "Solid-group extraction should preserve the first file");
  DEVZIP_REQUIRE(read_text(extracted / "beta.txt") == "shared line one\nshared line two\nshared line three\n",
                 "Solid-group extraction should preserve the second file");
}

DEVZIP_TEST(generic_binary_files_can_share_a_solid_block_without_chunking) {
  const auto root = make_temp_root("generic-solid-group");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  const auto extracted = root / "extracted";

  std::filesystem::create_directories(source);

  std::vector<std::byte> payload;
  payload.reserve(512 * 1024);
  std::array<std::byte, 256> pattern{};
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    pattern[index] = static_cast<std::byte>(index & 0xFF);
  }
  for (int index = 0; index < 4096; ++index) {
    payload.insert(payload.end(), pattern.begin(), pattern.end());
  }

  write_bytes(source / "blob.bin", payload);

  InflatingBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);

  DEVZIP_REQUIRE(manifest.blocks.size() == 1, "Generic binary payloads should share one solid block");
  DEVZIP_REQUIRE(manifest.entries.back().block_spans.size() == 1,
                 "Unchunked generic binaries should map to one span within the shared solid block");

  devzip::verify_archive(archive, backend);
  devzip::extract_archive(archive, extracted, backend);

  DEVZIP_REQUIRE(read_bytes(extracted / "blob.bin") == payload,
                 "Solid-group extraction should preserve generic binary payloads");
}
