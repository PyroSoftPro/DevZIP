#pragma once

#include <filesystem>

namespace devzip {

class TempFileGuard {
 public:
  explicit TempFileGuard(std::filesystem::path final_path);
  ~TempFileGuard();

  const std::filesystem::path& temp_path() const { return temp_path_; }
  void commit();

 private:
  std::filesystem::path final_path_;
  std::filesystem::path temp_path_;
  bool committed_ = false;
};

}  // namespace devzip
