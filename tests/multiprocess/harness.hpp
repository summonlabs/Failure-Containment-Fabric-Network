// FCFN multiprocess proof harness.
//
// Everything here uses real independent OS processes and real loopback sockets:
// fcfn_coordinator runs as its own process, fcfn_client is a separate process
// that owns a session, and hard kills are delivered with TerminateProcess.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_TEST_MULTIPROCESS_HARNESS_HPP
#define FCFN_TEST_MULTIPROCESS_HARNESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "process.hpp"
#include "temp_dir.hpp"
#include "test_harness.hpp"

namespace fcfn::test {

/// A coordinator running as its own process.
class Coordinator {
 public:
  Coordinator() = default;
  Coordinator(Coordinator&&) = default;
  Coordinator& operator=(Coordinator&&) = default;

  /// Start a coordinator. Throws TestFailure when it does not become ready.
  static Coordinator start(const std::filesystem::path& store_root, const std::string& token,
                           const std::vector<std::string>& extra_arguments = {});

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string& startup_json() const noexcept { return startup_; }
  [[nodiscard]] bool running() { return process_.running(); }

  /// Close stdin (a waiting crash point observes EOF) and hard kill.
  void kill();
  /// Hard kill without touching stdin: the process dies exactly where it is.
  void kill_now();
  /// Wait for an arbitrary marker line on the coordinator's stdout.
  bool wait_for_line(const std::string& prefix, std::string& line);
  /// Everything the coordinator has printed so far.
  [[nodiscard]] std::string output() const { return process_.output(); }
  /// Ask the process to exit cleanly by closing its stdin and waiting.
  int wait();

 private:
  ChildProcess process_{};
  std::uint16_t port_{0};
  std::string startup_{};
};

struct ClientRun {
  int exit_code{0};
  std::string output{};
};

/// Run one client session to completion. Never uses a timeout.
[[nodiscard]] ClientRun run_client(std::uint16_t port, const std::string& token,
                                   const std::filesystem::path& scenario);

/// Launch a client session without waiting (used to kill the coordinator while
/// the client is blocked waiting for its answer).
[[nodiscard]] ChildProcess launch_client(std::uint16_t port, const std::string& token,
                                         const std::filesystem::path& scenario);

/// Write a text file, failing the test when the write does not succeed.
void write_text_file(const std::filesystem::path& path, const std::string& contents);

/// True when the text contains the given substring.
[[nodiscard]] bool contains(const std::string& text, const std::string& needle);

/// Count the occurrences of a substring.
[[nodiscard]] std::size_t count_of(const std::string& text, const std::string& needle);

/// Extract the value of a JSON field from a deterministic document.
[[nodiscard]] std::string json_field(const std::string& document, const std::string& key);

/// Extract the status token of the step mentioning the given verb.
[[nodiscard]] std::string step_status(const std::string& output, const std::string& verb);

}  // namespace fcfn::test

#endif  // FCFN_TEST_MULTIPROCESS_HARNESS_HPP
