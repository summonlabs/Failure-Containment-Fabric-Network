// FCFN test support: real OS processes.
//
// Multiprocess proof requires real independent processes that can be hard-killed
// at meaningful boundaries, not threads. This wrapper spawns a child process with
// piped stdio, waits for marker lines, terminates it without notice, and reports
// the exit status.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_PROCESS_HPP
#define FCFN_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fcfn::test {

/// Directory holding the built FCFN executables.
[[nodiscard]] std::filesystem::path app_directory();
/// Build root of the current test run.
[[nodiscard]] std::filesystem::path build_root();

struct ProcessOptions {
  std::filesystem::path executable{};
  std::vector<std::string> arguments{};
  std::filesystem::path working_directory{};
  std::vector<std::pair<std::string, std::string>> environment{};
};

/// A spawned child process. Never uses threads, sleeps, or timeouts.
class ChildProcess {
 public:
  ChildProcess();
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Spawn the child. Throws TestFailure when the process cannot be created.
  static ChildProcess spawn(const ProcessOptions& options);

  [[nodiscard]] bool running();
  [[nodiscard]] std::uint32_t id() const noexcept;

  /// Block until a line starting with the prefix arrives or stdout closes.
  /// Returns false when the stream ended without the marker.
  bool wait_for_line(const std::string& prefix, std::string& line);

  /// Read whatever is already buffered without blocking for new bytes.
  [[nodiscard]] std::string drain_available();

  /// Terminate without notice (SIGKILL / TerminateProcess) and reap.
  void kill_hard();

  /// Close the child's stdin. A child waiting on stdin observes EOF.
  void close_stdin();

  /// Wait for natural exit and return the exit code.
  int wait();

  /// True once the process has exited.
  [[nodiscard]] bool exited();

  /// Every line observed so far.
  [[nodiscard]] std::string output() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Wait until a file exists (used to observe a child's durable progress).
[[nodiscard]] bool wait_for_file(const std::filesystem::path& path);

/// Wait until a file contains the marker text.
[[nodiscard]] bool wait_for_file_contains(const std::filesystem::path& path, const std::string& marker);

}  // namespace fcfn::test

#endif  // FCFN_TEST_PROCESS_HPP
