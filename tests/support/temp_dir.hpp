// FCFN test support: temporary directories with deterministic cleanup.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_TEMP_DIR_HPP
#define FCFN_TEST_TEMP_DIR_HPP

#include <filesystem>
#include <string>

namespace fcfn::test {

/// Unique temporary directory removed recursively on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(const std::string& name) const { return path_ / name; }
  /// Remove the contents but keep the directory (used to simulate a fresh root).
  void clear() const;
  /// Detach the directory so it survives destruction (crash-inspection tests).
  void release() noexcept { released_ = true; }

 private:
  std::filesystem::path path_;
  bool released_{false};
};

/// Root directory for induced-crash artifacts supplied by the test runner.
[[nodiscard]] std::filesystem::path artifact_root();

}  // namespace fcfn::test

#endif  // FCFN_TEST_TEMP_DIR_HPP
