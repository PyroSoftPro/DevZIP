#include "devzip/source_scanner.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace devzip {
namespace {

struct NativeAttributes {
  std::uint32_t value = 0;
  bool is_hidden = false;
  bool is_reparse_point = false;
};

std::string to_lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now_time);
#else
  gmtime_r(&now_time, &utc);
#endif
  std::ostringstream stream;
  stream << (utc.tm_year + 1900) << '-';
  stream.width(2);
  stream.fill('0');
  stream << (utc.tm_mon + 1) << '-';
  stream.width(2);
  stream << utc.tm_mday << 'T';
  stream.width(2);
  stream << utc.tm_hour << ':';
  stream.width(2);
  stream << utc.tm_min << ':';
  stream.width(2);
  stream << utc.tm_sec << 'Z';
  return stream.str();
}

NativeAttributes read_native_attributes(const std::filesystem::path& path) {
  NativeAttributes attributes{};
#if defined(_WIN32)
  const DWORD raw = GetFileAttributesW(path.c_str());
  if (raw != INVALID_FILE_ATTRIBUTES) {
    attributes.value = raw;
    attributes.is_hidden = (raw & FILE_ATTRIBUTE_HIDDEN) != 0;
    attributes.is_reparse_point = (raw & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  }
#endif
  return attributes;
}

std::uint64_t modified_time_ns(const std::filesystem::path& path) {
  std::error_code error;
  const auto timestamp = std::filesystem::last_write_time(path, error);
  if (error) {
    return 0;
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch());
  return static_cast<std::uint64_t>(ns.count());
}

std::uint64_t file_size_bytes(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

std::string archive_path_for(const std::filesystem::path& root, const std::filesystem::path& path) {
  if (root == path && std::filesystem::is_directory(path)) {
    return ".";
  }
  const auto relative = std::filesystem::relative(path, root);
  return relative.empty() ? path_to_generic_utf8(path.filename()) : path_to_generic_utf8(relative);
}

class Scanner {
 public:
  Scanner(std::filesystem::path root, ScanOptions options)
      : root_(std::move(root)), options_(options) {
    manifest_.backend.name = "scan-only";
    manifest_.backend.version = "0";
    manifest_.source_name = path_to_generic_utf8(root_.filename());
    manifest_.created_utc = utc_timestamp();
  }

  ScanResult scan() {
    scan_path(root_, 0);
    sort_entries();
    return ScanResult{root_, manifest_, skipped_paths_};
  }

 private:
  void scan_path(const std::filesystem::path& path, std::size_t depth) {
    if (depth > options_.maximum_depth) {
      throw std::runtime_error("Exceeded maximum traversal depth while scanning " + path_to_utf8(path));
    }

    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
      handle_failure(path, error.message());
      return;
    }

    const auto attributes = read_native_attributes(path);
    if (!options_.include_hidden && attributes.is_hidden) {
      skipped_paths_.push_back(path);
      return;
    }

    if (std::filesystem::is_directory(status)) {
      append_entry(path, EntryKind::Directory, attributes, 0);

      if (attributes.is_reparse_point && !options_.follow_reparse_points) {
        skipped_paths_.push_back(path);
        return;
      }

      const auto identity = canonical_identity(path);
      if (!visited_directories_.insert(identity).second) {
        skipped_paths_.push_back(path);
        return;
      }

      std::vector<std::filesystem::path> children;
      for (std::filesystem::directory_iterator iterator(
               path, std::filesystem::directory_options::skip_permission_denied, error);
           !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        children.push_back(iterator->path());
      }
      if (error) {
        handle_failure(path, error.message());
        return;
      }

      std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
        return to_lower_ascii(path_to_generic_utf8(left)) < to_lower_ascii(path_to_generic_utf8(right));
      });

      for (const auto& child : children) {
        scan_path(child, depth + 1);
      }
      return;
    }

    if (std::filesystem::is_regular_file(status)) {
      append_entry(path, EntryKind::File, attributes, file_size_bytes(path));
      return;
    }

    skipped_paths_.push_back(path);
  }

  void append_entry(const std::filesystem::path& path,
                    EntryKind kind,
                    const NativeAttributes& attributes,
                    std::uint64_t size) {
    ManifestEntry entry;
    entry.archive_path = archive_path_for(root_, path);
    entry.kind = kind;
    entry.size = size;
    entry.modified_time_ns = modified_time_ns(path);
    entry.windows_attributes = attributes.value;
    manifest_.entries.push_back(std::move(entry));
  }

  std::string canonical_identity(const std::filesystem::path& path) const {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path_to_generic_utf8(path.lexically_normal()) : path_to_generic_utf8(canonical);
  }

  void handle_failure(const std::filesystem::path& path, const std::string& message) {
    if (options_.fail_on_access_denied) {
      throw std::runtime_error("Failed to scan " + path_to_utf8(path) + ": " + message);
    }
    skipped_paths_.push_back(path);
  }

  void sort_entries() {
    std::sort(manifest_.entries.begin(), manifest_.entries.end(), [](const auto& left, const auto& right) {
      const auto left_key = to_lower_ascii(left.archive_path);
      const auto right_key = to_lower_ascii(right.archive_path);
      if (left_key == right_key) {
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
      }
      return left_key < right_key;
    });
  }

  std::filesystem::path root_;
  ScanOptions options_;
  ArchiveManifest manifest_;
  std::vector<std::filesystem::path> skipped_paths_;
  std::unordered_set<std::string> visited_directories_;
};

}  // namespace

ScanResult scan_source_tree(const std::filesystem::path& source_root, const ScanOptions& options) {
  if (!std::filesystem::exists(source_root)) {
    throw std::runtime_error("Source path does not exist: " + path_to_utf8(source_root));
  }

  Scanner scanner(source_root, options);
  return scanner.scan();
}

}  // namespace devzip
