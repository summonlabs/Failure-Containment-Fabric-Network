// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fcfn::test {
namespace {

std::uint64_t g_seed = 20260101;
std::string g_filter;
std::string g_data_directory{"."};

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string suite, std::string name, void (*function)()) {
  cases_.push_back(TestCase{std::move(suite), std::move(name), function});
}

std::uint64_t seed() { return g_seed; }
const std::string& filter() { return g_filter; }
const std::string& data_directory() { return g_data_directory; }

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw TestFailure(stream.str());
}

int run_all(int argc, char** argv) {
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const char* argument = argv[i];
    if (std::strncmp(argument, "--seed=", 7) == 0) {
      if (!parse_u64(argument + 7, g_seed)) {
        std::fprintf(stderr, "invalid --seed value\n");
        return 2;
      }
    } else if (std::strncmp(argument, "--filter=", 9) == 0) {
      g_filter = argument + 9;
    } else if (std::strncmp(argument, "--data-dir=", 11) == 0) {
      g_data_directory = argument + 11;
    } else if (std::strcmp(argument, "--list") == 0) {
      list_only = true;
    } else {
      std::fprintf(stderr, "unrecognised argument: %s\n", argument);
      return 2;
    }
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : Registry::instance().cases()) {
    const std::string full = test.suite + "." + test.name;
    if (!g_filter.empty() && full.find(g_filter) == std::string::npos) {
      continue;
    }
    selected.push_back(test);
  }
  std::sort(selected.begin(), selected.end(), [](const TestCase& a, const TestCase& b) {
    return a.suite != b.suite ? a.suite < b.suite : a.name < b.name;
  });

  if (list_only) {
    for (const TestCase& test : selected) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::printf("seed=%llu cases=%zu\n", static_cast<unsigned long long>(g_seed), selected.size());
  std::fflush(stdout);
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : selected) {
    const auto start = std::chrono::steady_clock::now();
    try {
      test.function();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      std::printf("PASS %s.%s (%lld ms)\n", test.suite.c_str(), test.name.c_str(),
                  static_cast<long long>(elapsed));
      std::fflush(stdout);
      ++passed;
    } catch (const TestFailure& failure) {
      std::printf("FAIL %s.%s\n  %s\n", test.suite.c_str(), test.name.c_str(), failure.what());
      std::printf("  reproduce with --seed=%llu --filter=%s.%s\n",
                  static_cast<unsigned long long>(g_seed), test.suite.c_str(), test.name.c_str());
      std::fflush(stdout);
      ++failed;
    } catch (const std::exception& error) {
      std::printf("FAIL %s.%s\n  unexpected exception: %s\n", test.suite.c_str(), test.name.c_str(),
                  error.what());
      ++failed;
    } catch (...) {
      std::printf("FAIL %s.%s\n  unexpected non-standard exception\n", test.suite.c_str(),
                  test.name.c_str());
      ++failed;
    }
  }
  std::printf("summary passed=%zu failed=%zu\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace fcfn::test

int main(int argc, char** argv) { return ::fcfn::test::run_all(argc, argv); }
