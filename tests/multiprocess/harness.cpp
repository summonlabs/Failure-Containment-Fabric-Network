// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "harness.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace fcfn::test {
namespace {

constexpr const char* kToken = "fcfn-multiprocess-token";

std::filesystem::path executable(const char* name) {
#ifdef _WIN32
  return app_directory() / (std::string{name} + ".exe");
#else
  return app_directory() / name;
#endif
}

}  // namespace

Coordinator Coordinator::start(const std::filesystem::path& store_root, const std::string& token,
                               const std::vector<std::string>& extra_arguments) {
  ProcessOptions options;
  options.executable = executable("fcfn_coordinator");
  options.arguments = {"--store", store_root.string(), "--token", token, "--port", "0"};
  for (const std::string& argument : extra_arguments) {
    options.arguments.push_back(argument);
  }
  Coordinator coordinator;
  coordinator.process_ = ChildProcess::spawn(options);

  std::string line;
  if (!coordinator.process_.wait_for_line("STARTUP ", line)) {
    throw TestFailure("coordinator did not report its startup state: " + coordinator.process_.output());
  }
  coordinator.startup_ = line;
  if (!coordinator.process_.wait_for_line("LISTENING ", line)) {
    throw TestFailure("coordinator did not report a listening port: " + coordinator.process_.output());
  }
  const std::size_t space = line.find(' ');
  if (space == std::string::npos) {
    throw TestFailure("coordinator port line is malformed");
  }
  coordinator.port_ = static_cast<std::uint16_t>(std::stoul(line.substr(space + 1)));
  if (!coordinator.process_.wait_for_line("READY", line)) {
    throw TestFailure("coordinator never became ready");
  }
  return coordinator;
}

void Coordinator::kill() {
  process_.close_stdin();
  process_.kill_hard();
}

void Coordinator::kill_now() { process_.kill_hard(); }

bool Coordinator::wait_for_line(const std::string& prefix, std::string& line) {
  return process_.wait_for_line(prefix, line);
}

int Coordinator::wait() {
  process_.close_stdin();
  return process_.wait();
}

ClientRun run_client(std::uint16_t port, const std::string& token,
                     const std::filesystem::path& scenario) {
  ProcessOptions options;
  options.executable = executable("fcfn_client");
  options.arguments = {"--port", std::to_string(port), "--token", token, "--scenario",
                       scenario.string()};
  ChildProcess process = ChildProcess::spawn(options);
  ClientRun run;
  // Read until the client closes its stdout. Waiting first would deadlock as
  // soon as the client filled the pipe buffer with JSON.
  std::string unused;
  (void)process.wait_for_line("\x01 never matches", unused);
  run.exit_code = process.wait();
  run.output = process.output();
  return run;
}

ChildProcess launch_client(std::uint16_t port, const std::string& token,
                           const std::filesystem::path& scenario) {
  ProcessOptions options;
  options.executable = executable("fcfn_client");
  options.arguments = {"--port", std::to_string(port), "--token", token, "--scenario",
                       scenario.string()};
  return ChildProcess::spawn(options);
}

void write_text_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw TestFailure("cannot write " + path.string());
  }
  stream << contents;
  stream.flush();
  if (!stream) {
    throw TestFailure("write failed for " + path.string());
  }
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t position = text.find(needle);
  while (position != std::string::npos) {
    ++count;
    position = text.find(needle, position + needle.size());
  }
  return count;
}

std::string json_field(const std::string& document, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t position = document.find(needle);
  if (position == std::string::npos) {
    return {};
  }
  std::size_t cursor = position + needle.size();
  if (cursor >= document.size()) {
    return {};
  }
  if (document[cursor] == '"') {
    const std::size_t end = document.find('"', cursor + 1);
    if (end == std::string::npos) {
      return {};
    }
    return document.substr(cursor + 1, end - cursor - 1);
  }
  std::size_t end = cursor;
  while (end < document.size() && document[end] != ',' && document[end] != '}') {
    ++end;
  }
  return document.substr(cursor, end - cursor);
}

std::string step_status(const std::string& output, const std::string& verb) {
  std::istringstream stream(output);
  std::string line;
  std::string result;
  while (std::getline(stream, line)) {
    if (line.rfind("STEP ", 0) != 0) {
      continue;
    }
    const std::size_t verb_position = line.find(" " + verb + " ");
    if (verb_position == std::string::npos) {
      continue;
    }
    const std::size_t status_position = line.find("status=");
    if (status_position == std::string::npos) {
      continue;
    }
    std::size_t end = line.find(' ', status_position);
    if (end == std::string::npos) {
      end = line.size();
    }
    result = line.substr(status_position + 7, end - status_position - 7);
  }
  return result;
}

}  // namespace fcfn::test
