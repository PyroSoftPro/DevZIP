#include "devzip/temp_file_guard.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace devzip {

TempFileGuard::TempFileGuard(std::filesystem::path final_path)
    : final_path_(std::move(final_path)) {
  std::filesystem::path temp_name = final_path_.filename();
  temp_name += ".";
  temp_name += std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  temp_name += ".tmp";
  temp_path_ = final_path_.parent_path() / temp_name;
}

TempFileGuard::~TempFileGuard() {
  if (!committed_) {
    std::error_code error;
    std::filesystem::remove(temp_path_, error);
  }
}

void TempFileGuard::commit() {
  std::filesystem::create_directories(final_path_.parent_path());
#if defined(_WIN32)
  if (!MoveFileExW(temp_path_.c_str(),
                   final_path_.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::filesystem::filesystem_error(
        "Failed to commit temporary file",
        temp_path_,
        final_path_,
        std::error_code(static_cast<int>(GetLastError()), std::system_category()));
  }
#else
  std::error_code error;
  std::filesystem::rename(temp_path_, final_path_, error);
  if (error) {
    throw std::filesystem::filesystem_error("Failed to commit temporary file",
                                            temp_path_,
                                            final_path_,
                                            error);
  }
#endif
  committed_ = true;
}

}  // namespace devzip
