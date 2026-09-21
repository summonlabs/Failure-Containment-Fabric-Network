// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "temp_dir.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <system_error>

#include "test_harness.hpp"

namespace fcfn::test {
namespace {

std::atomic<std::uint64_t> g_counter{0};

std::filesystem::path base_directory() {
  const char* from_environment = std::getenv("FCFN_TEST_TMP");
  if (from_environment != nullptr && *from_environment != '\0') {
    return std::filesystem::path{from_environment};
  }
  return std::filesystem::temp_directory_path();
}

}  // namespace

TempDir::TempDir(const std::string& label) {
  const auto stamp = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t counter = g_counter.fetch_add(1);
  path_ = base_directory() / ("fcfn-" + label + "-" + std::to_string(stamp) + "-" +
                              std::to_string(counter));
  std::error_code error;
  std::filesystem::create_directories(path_, error);
  if (error) {
    throw TestFailure("cannot create temporary directory " + path_.string());
  }
}

TempDir::~TempDir() {
  if (released_) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

void TempDir::clear() const {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path_, error)) {
    std::filesystem::remove_all(entry.path(), error);
  }
  if (error) {
    throw TestFailure("cannot clear temporary directory " + path_.string());
  }
}

std::filesystem::path artifact_root() {
  const char* from_environment = std::getenv("FCFN_TEST_ARTIFACTS");
  if (from_environment != nullptr && *from_environment != '\0') {
    return std::filesystem::path{from_environment};
  }
  return base_directory() / "fcfn-artifacts";
}

}  // namespace fcfn::test
