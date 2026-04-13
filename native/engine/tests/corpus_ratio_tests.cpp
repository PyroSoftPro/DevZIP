#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

DEVZIP_TEST(corpus_manifest_contains_required_tools) {
  const auto manifest_path =
      std::filesystem::path("..") / ".." / ".." / "benchmarks" / "manifests" / "mixed-large.json";
  const std::string content = read_file(manifest_path);

  DEVZIP_REQUIRE(!content.empty(), "Benchmark manifest should exist and be non-empty");
  DEVZIP_REQUIRE(content.find("\"devzip-native\"") != std::string::npos,
                 "Manifest should define the native DevZip shipping lane");
  DEVZIP_REQUIRE(content.find("\"7z-lzma2\"") != std::string::npos,
                 "Manifest should define the 7-Zip LZMA2 lane");
  DEVZIP_REQUIRE(content.find("\"7z-lzma\"") != std::string::npos,
                 "Manifest should define the 7-Zip LZMA lane");
  DEVZIP_REQUIRE(content.find("\"7z-ppmd\"") != std::string::npos,
                 "Manifest should define the 7-Zip PPMd lane");
  DEVZIP_REQUIRE(content.find("\"7z-bzip2\"") != std::string::npos,
                 "Manifest should define the 7-Zip BZip2 lane");
  DEVZIP_REQUIRE(content.find("\"7z-deflate\"") != std::string::npos,
                 "Manifest should define the 7-Zip Deflate lane");
  DEVZIP_REQUIRE(content.find("\"winrar\"") != std::string::npos, "Manifest should define WinRAR");
  DEVZIP_REQUIRE(content.find("\"windows-zip\"") != std::string::npos,
                 "Manifest should define the native Windows ZIP baseline");
  DEVZIP_REQUIRE(content.find("\"devzip-reference\"") == std::string::npos,
                 "Manifest should not define the reference DevZip lane");
  DEVZIP_REQUIRE(content.find("\"winzip\"") == std::string::npos,
                 "Manifest should not define WinZip");
}

DEVZIP_TEST(corpus_manifest_tracks_win_goal) {
  const auto manifest_path =
      std::filesystem::path("..") / ".." / ".." / "benchmarks" / "manifests" / "mixed-large.json";
  const std::string content = read_file(manifest_path);

  DEVZIP_REQUIRE(content.find("\"aggregate_minimum_win_percent_vs_7z\"") != std::string::npos,
                 "Manifest should define the minimum aggregate win target");
  DEVZIP_REQUIRE(content.find("\"shipping_tool\"") != std::string::npos,
                 "Manifest should record the shipping tool lane");
  DEVZIP_REQUIRE(content.find("\"shipping_backend\"") != std::string::npos,
                 "Manifest should record the intended shipping backend");
}
