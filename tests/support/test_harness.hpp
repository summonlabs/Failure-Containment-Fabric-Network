// FCFN test harness.
//
// Intentional properties:
//   * no timeouts of any kind: a hanging test is a defect, not something to hide
//   * per-test isolation through exceptions, so one failure does not mask others
//   * deterministic seeds printed on failure for exact reproduction
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_HARNESS_HPP
#define FCFN_TEST_HARNESS_HPP

#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "fcfn/core/result.hpp"
#include "fcfn/core/status.hpp"

namespace fcfn::test {

class TestFailure : public std::exception {
 public:
  explicit TestFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

class Registry {
 public:
  static Registry& instance();
  void add(std::string suite, std::string name, void (*function)());
  [[nodiscard]] const std::vector<TestCase>& cases() const { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) {
    Registry::instance().add(suite, name, function);
  }
};

/// Seed shared by property tests. Overridable with --seed=<n>.
[[nodiscard]] std::uint64_t seed();
/// Optional --filter=<substring> selection.
[[nodiscard]] const std::string& filter();
/// Suite data directory supplied with --data-dir=<path>.
[[nodiscard]] const std::string& data_directory();

[[noreturn]] void fail(const char* file, int line, const std::string& message);

template <class T>
std::string describe(const T& value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

inline std::string describe(const Status& status) { return status.to_string(); }

inline std::string describe(const VoidResult& result) { return result.status().to_string(); }

int run_all(int argc, char** argv);

}  // namespace fcfn::test

#define FCFN_TEST(suite_name, test_name)                                                       \
  static void fcfn_test_##suite_name##_##test_name();                                          \
  static const ::fcfn::test::Registrar fcfn_registrar_##suite_name##_##test_name(               \
      #suite_name, #test_name, &fcfn_test_##suite_name##_##test_name);                          \
  static void fcfn_test_##suite_name##_##test_name()

#define FCFN_CHECK(condition)                                                                   \
  do {                                                                                          \
    if (!(condition)) {                                                                         \
      ::fcfn::test::fail(__FILE__, __LINE__, std::string("check failed: ") + #condition);        \
    }                                                                                           \
  } while (false)

#define FCFN_CHECK_EQ(actual, expected)                                                         \
  do {                                                                                          \
    const auto& fcfn_actual = (actual);                                                          \
    const auto& fcfn_expected = (expected);                                                       \
    if (!(fcfn_actual == fcfn_expected)) {                                                       \
      ::fcfn::test::fail(__FILE__, __LINE__,                                                     \
                         std::string("expected ") + #actual + " == " + #expected +              \
                             "\n  actual:   " + ::fcfn::test::describe(fcfn_actual) +          \
                             "\n  expected: " + ::fcfn::test::describe(fcfn_expected));         \
    }                                                                                           \
  } while (false)

#define FCFN_CHECK_NE(actual, unexpected)                                                       \
  do {                                                                                          \
    const auto& fcfn_actual = (actual);                                                          \
    if (fcfn_actual == (unexpected)) {                                                          \
      ::fcfn::test::fail(__FILE__, __LINE__, std::string("expected ") + #actual + " != " + #unexpected); \
    }                                                                                           \
  } while (false)

#define FCFN_CHECK_STATUS(expr, expected_code)                                                  \
  do {                                                                                          \
    const auto& fcfn_result = (expr);                                                            \
    if (fcfn_result.status().code() != (expected_code)) {                                        \
      ::fcfn::test::fail(__FILE__, __LINE__,                                                     \
                         std::string("expected status ") + #expected_code + " from " + #expr +   \
                             " but got " + fcfn_result.status().to_string());                   \
    }                                                                                           \
  } while (false)

#define FCFN_CHECK_OK(expr) FCFN_CHECK_STATUS(expr, ::fcfn::StatusCode::Ok)

#define FCFN_CHECK_FAILS(expr)                                                                  \
  do {                                                                                          \
    const auto& fcfn_result = (expr);                                                            \
    if (fcfn_result.status().ok()) {                                                             \
      ::fcfn::test::fail(__FILE__, __LINE__, std::string("expected failure from ") + #expr);     \
    }                                                                                           \
  } while (false)

#endif  // FCFN_TEST_HARNESS_HPP
