#include "devzip/backend.h"
#include "devzip/extractor.h"
#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
  auto root = std::filesystem::temp_directory_path() / ("devzip-routing-" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

class FailingIfCompressedBackend final : public devzip::CompressionBackend {
 public:
  devzip::BackendStamp stamp() const override { return {"routing-skip", "1"}; }

  devzip::CompressionResponse compress(const devzip::CompressionRequest&) override {
    throw std::runtime_error("Compression should have been skipped");
  }

  std::vector<std::byte> decompress(std::span<const std::byte>,
                                    std::uint64_t /*expected_raw_size*/) const override {
    throw std::runtime_error("Compressed blocks are not expected in this test");
  }
};

class CountingBackend final : public devzip::CompressionBackend {
 public:
  int compress_calls = 0;

  devzip::BackendStamp stamp() const override { return {"routing-count", "1"}; }

  devzip::CompressionResponse compress(const devzip::CompressionRequest& request) override {
    ++compress_calls;

    devzip::CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = devzip::checksum_bytes(request.input);
    response.encoded.assign(request.input.begin(), request.input.end());
    response.encoded.push_back(std::byte{0});
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte>,
                                    std::uint64_t /*expected_raw_size*/) const override {
    throw std::runtime_error("Compressed blocks are not expected in this test");
  }
};

}  // namespace

DEVZIP_TEST(routing_skips_backend_for_mp4_files) {
  const auto root = make_temp_root("mp4");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";

  const std::vector<std::byte> mp4_like = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x18},
      std::byte{'f'}, std::byte{'t'}, std::byte{'y'}, std::byte{'p'},
      std::byte{'m'}, std::byte{'p'}, std::byte{'4'}, std::byte{'2'},
  };

  write_bytes(source / "clip.mp4", mp4_like);

  FailingIfCompressedBackend backend;
  devzip::create_archive(source, archive, backend);
  const auto manifest = devzip::read_archive_manifest(archive);

  DEVZIP_REQUIRE(manifest.blocks.size() == 1, "MP4 fixtures should create one stored block");
  DEVZIP_REQUIRE(manifest.blocks.front().storage == devzip::BlockStorageKind::StoredRaw,
                 "MP4 fixtures should bypass backend compression");
}

DEVZIP_TEST(routing_still_uses_backend_for_text_files) {
  const auto root = make_temp_root("text");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";

  const std::string text = "alpha alpha alpha alpha alpha\n";
  write_bytes(source / "notes.txt", std::as_bytes(std::span(text)));

  CountingBackend backend;
  devzip::create_archive(source, archive, backend);

  DEVZIP_REQUIRE(backend.compress_calls > 0, "Text files should still be offered to the backend");
}

DEVZIP_TEST(routing_still_uses_backend_for_png_files) {
  const auto root = make_temp_root("png");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";

  const std::vector<std::byte> png_like = {
      std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0D},
      std::byte{0x49}, std::byte{0x48}, std::byte{0x44}, std::byte{0x52},
  };

  write_bytes(source / "image.png", png_like);

  CountingBackend backend;
  devzip::create_archive(source, archive, backend);

  DEVZIP_REQUIRE(backend.compress_calls > 0, "PNG files should remain backend candidates");
}

DEVZIP_TEST(routing_still_uses_backend_for_jpeg_files) {
  const auto root = make_temp_root("jpeg");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";

  const std::vector<std::byte> jpeg_like = {
      std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE0},
      std::byte{0x00}, std::byte{0x10}, std::byte{'J'}, std::byte{'F'},
      std::byte{'I'}, std::byte{'F'}, std::byte{0x00},
  };

  write_bytes(source / "image.jpg", jpeg_like);

  CountingBackend backend;
  devzip::create_archive(source, archive, backend);

  DEVZIP_REQUIRE(backend.compress_calls > 0, "JPEG files should remain backend candidates");
}

DEVZIP_TEST(routing_still_uses_backend_for_zip_files) {
  const auto root = make_temp_root("zip");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";

  const std::vector<std::byte> zip_like = {
      std::byte{'P'}, std::byte{'K'}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x14}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };

  write_bytes(source / "bundle.zip", zip_like);

  CountingBackend backend;
  devzip::create_archive(source, archive, backend);

  DEVZIP_REQUIRE(backend.compress_calls > 0, "Archive containers should remain backend candidates");
}
