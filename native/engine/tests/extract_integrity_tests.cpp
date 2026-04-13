#include "devzip/backend.h"
#include "devzip/extractor.h"
#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
  auto root = std::filesystem::temp_directory_path() / ("devzip-integrity-" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
}

}  // namespace

DEVZIP_TEST(corrupted_manifest_is_rejected_during_verify) {
  const auto root = make_temp_root("manifest");
  const auto source = root / "source";
  const auto archive = root / "sample.dvz";
  std::filesystem::create_directories(source);
  write_text(source / "a.txt", "hello world");

  auto backend = devzip::make_libzpaq_backend();
  devzip::create_archive(source, archive, *backend);

  {
    std::ifstream input(archive, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bytes[40] ^= 0x01;
    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  std::string message;
  try {
    devzip::verify_archive(archive, *backend);
  } catch (const std::exception& exception) {
    message = exception.what();
  }

  DEVZIP_REQUIRE(message.find("Manifest checksum mismatch") != std::string::npos,
                 "Corrupted manifests should fail with a checksum mismatch");
}

DEVZIP_TEST(invalid_magic_is_rejected_during_read) {
  const auto root = make_temp_root("magic");
  const auto archive = root / "bad.dvz";
  std::string bytes(40, '\0');
  bytes[0] = 'B';
  bytes[1] = 'A';
  bytes[2] = 'D';
  bytes[3] = '!';
  write_text(archive, bytes);

  std::string message;
  try {
    (void)devzip::read_archive_manifest(archive);
  } catch (const std::exception& exception) {
    message = exception.what();
  }

  DEVZIP_REQUIRE(message.find("Not a DVZ archive") != std::string::npos,
                 "Invalid archive magic should be rejected explicitly");
}

DEVZIP_TEST(unknown_backend_is_rejected_during_factory_resolution) {
  std::string message;
  try {
    (void)devzip::make_backend({"unknown-backend", "1"});
  } catch (const std::exception& exception) {
    message = exception.what();
  }

  DEVZIP_REQUIRE(message.find("Unsupported compression backend") != std::string::npos,
                 "Unknown backend ids should fail clearly");
}
