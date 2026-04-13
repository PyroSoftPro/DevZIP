#include "devzip/temp_file_guard.h"
#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path make_temp_root(const std::string& name) {
  auto root = std::filesystem::temp_directory_path() / ("devzip-cleanup-" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

DEVZIP_TEST(temp_file_guard_cleans_up_uncommitted_temp_files) {
  const auto root = make_temp_root("cleanup");
  const auto final_path = root / "archive.dvz";
  std::filesystem::path temp_path;

  {
    devzip::TempFileGuard guard(final_path);
    temp_path = guard.temp_path();
    std::ofstream output(temp_path, std::ios::binary);
    output << "partial";
  }

  DEVZIP_REQUIRE(!std::filesystem::exists(temp_path), "Temporary files should be removed on failure");
  DEVZIP_REQUIRE(!std::filesystem::exists(final_path), "Final file should not exist when commit never happened");
}

DEVZIP_TEST(temp_file_guard_replaces_existing_final_file) {
  const auto root = make_temp_root("replace");
  const auto final_path = root / "archive.dvz";
  {
    std::ofstream output(final_path, std::ios::binary);
    output << "old";
  }

  {
    devzip::TempFileGuard guard(final_path);
    std::ofstream output(guard.temp_path(), std::ios::binary);
    output << "new";
    output.close();
    guard.commit();
  }

  DEVZIP_REQUIRE(read_text(final_path) == "new", "Committed temp file should replace the final file");
}
