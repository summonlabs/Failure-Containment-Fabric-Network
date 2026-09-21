// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "process.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#include "test_harness.hpp"

#ifndef FCFN_TEST_APP_DIR
#define FCFN_TEST_APP_DIR "."
#endif
#ifndef FCFN_TEST_BUILD_ROOT
#define FCFN_TEST_BUILD_ROOT "."
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fcfn::test {

std::filesystem::path app_directory() { return std::filesystem::path{FCFN_TEST_APP_DIR}; }
std::filesystem::path build_root() { return std::filesystem::path{FCFN_TEST_BUILD_ROOT}; }

#ifdef _WIN32

struct ChildProcess::Impl {
  HANDLE process{nullptr};
  HANDLE thread{nullptr};
  HANDLE stdin_write{nullptr};
  HANDLE stdout_read{nullptr};
  std::uint32_t pid{0};
  bool exited{false};
  int exit_code{0};
  std::string buffer;
  std::string output;
  bool stdout_closed{false};
};

namespace {

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string quote(const std::string& argument) {
  std::string out = "\"";
  for (const char c : argument) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

bool read_chunk(HANDLE handle, std::string& out) {
  char buffer[512];
  DWORD read = 0;
  if (::ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) {
    return false;
  }
  out.append(buffer, read);
  return true;
}

}  // namespace

ChildProcess::ChildProcess() = default;
ChildProcess::~ChildProcess() {
  if (impl_ && impl_->process != nullptr) {
    if (!impl_->exited) {
      ::TerminateProcess(impl_->process, 9);
      ::WaitForSingleObject(impl_->process, INFINITE);
    }
    ::CloseHandle(impl_->process);
    ::CloseHandle(impl_->thread);
  }
  if (impl_ && impl_->stdin_write != nullptr) {
    ::CloseHandle(impl_->stdin_write);
  }
  if (impl_ && impl_->stdout_read != nullptr) {
    ::CloseHandle(impl_->stdout_read);
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}
ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  impl_ = std::move(other.impl_);
  return *this;
}

ChildProcess ChildProcess::spawn(const ProcessOptions& options) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (::CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) == 0) {
    throw TestFailure("CreatePipe for stdin failed");
  }
  if (::CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0) == 0) {
    throw TestFailure("CreatePipe for stdout failed");
  }
  ::SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

  std::string command = quote(options.executable.string());
  for (const std::string& argument : options.arguments) {
    command += " ";
    command += quote(argument);
  }

  std::string environment;
  for (const auto& entry : options.environment) {
    environment += entry.first;
    environment += "=";
    environment += entry.second;
    environment += '\0';
  }
  environment += '\0';

  STARTUPINFOW startup{};
  startup.cb = sizeof(STARTUPINFOW);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_stdin_read;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;

  PROCESS_INFORMATION information{};
  std::wstring command_line = widen(command);
  std::wstring working = options.working_directory.empty() ? std::wstring{}
                                                           : widen(options.working_directory.string());
  const BOOL created = ::CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      options.environment.empty() ? nullptr : environment.data(),
      working.empty() ? nullptr : working.c_str(), &startup, &information);
  ::CloseHandle(child_stdin_read);
  ::CloseHandle(child_stdout_write);
  if (created == 0) {
    ::CloseHandle(child_stdin_write);
    ::CloseHandle(child_stdout_read);
    throw TestFailure("CreateProcess failed for " + options.executable.string());
  }

  ChildProcess child;
  child.impl_ = std::make_unique<Impl>();
  child.impl_->process = information.hProcess;
  child.impl_->thread = information.hThread;
  child.impl_->stdin_write = child_stdin_write;
  child.impl_->stdout_read = child_stdout_read;
  child.impl_->pid = information.dwProcessId;
  return child;
}

bool ChildProcess::running() { return !exited(); }

std::uint32_t ChildProcess::id() const noexcept { return impl_ ? impl_->pid : 0; }

bool ChildProcess::exited() {
  if (!impl_ || impl_->process == nullptr) {
    return true;
  }
  if (impl_->exited) {
    return true;
  }
  const DWORD status = ::WaitForSingleObject(impl_->process, 0);
  if (status == WAIT_OBJECT_0) {
    DWORD code = 0;
    ::GetExitCodeProcess(impl_->process, &code);
    impl_->exit_code = static_cast<int>(code);
    impl_->exited = true;
    return true;
  }
  return false;
}

bool ChildProcess::wait_for_line(const std::string& prefix, std::string& line) {
  if (!impl_) {
    return false;
  }
  for (;;) {
    const std::size_t newline = impl_->buffer.find('\n');
    if (newline != std::string::npos) {
      std::string candidate = impl_->buffer.substr(0, newline);
      impl_->buffer.erase(0, newline + 1);
      if (!candidate.empty() && candidate.back() == '\r') {
        candidate.pop_back();
      }
      impl_->output += candidate;
      impl_->output += '\n';
      if (candidate.rfind(prefix, 0) == 0) {
        line = candidate;
        return true;
      }
      continue;
    }
    if (impl_->stdout_closed) {
      return false;
    }
    if (!read_chunk(impl_->stdout_read, impl_->buffer)) {
      impl_->stdout_closed = true;
      if (!impl_->buffer.empty()) {
        line = impl_->buffer;
        impl_->output += impl_->buffer;
        impl_->buffer.clear();
        return line.rfind(prefix, 0) == 0;
      }
      return false;
    }
  }
}

std::string ChildProcess::drain_available() {
  if (!impl_ || impl_->stdout_closed) {
    return {};
  }
  std::string chunk;
  while (read_chunk(impl_->stdout_read, chunk)) {
  }
  impl_->output += chunk;
  return chunk;
}

void ChildProcess::kill_hard() {
  if (!impl_ || impl_->process == nullptr || impl_->exited) {
    return;
  }
  ::TerminateProcess(impl_->process, 9);
  ::WaitForSingleObject(impl_->process, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(impl_->process, &code);
  impl_->exit_code = static_cast<int>(code);
  impl_->exited = true;
}

void ChildProcess::close_stdin() {
  if (impl_ && impl_->stdin_write != nullptr) {
    ::CloseHandle(impl_->stdin_write);
    impl_->stdin_write = nullptr;
  }
}

std::string ChildProcess::output() const { return impl_ ? impl_->output : std::string{}; }

int ChildProcess::wait() {
  if (!impl_) {
    return -1;
  }
  if (!impl_->exited) {
    ::WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(impl_->process, &code);
    impl_->exit_code = static_cast<int>(code);
    impl_->exited = true;
  }
  return impl_->exit_code;
}

bool wait_for_file(const std::filesystem::path& path) {
  for (;;) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    if (error) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool wait_for_file_contains(const std::filesystem::path& path, const std::string& marker) {
  for (;;) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      std::ifstream stream(path, std::ios::binary);
      std::ostringstream contents;
      contents << stream.rdbuf();
      if (contents.str().find(marker) != std::string::npos) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

#else  // POSIX

struct ChildProcess::Impl {
  pid_t pid{-1};
  int stdin_write{-1};
  int stdout_read{-1};
  bool exited{false};
  int exit_code{0};
  std::string buffer;
  std::string output;
  bool stdout_closed{false};
};

ChildProcess::ChildProcess() = default;
ChildProcess::~ChildProcess() {
  if (impl_ && impl_->pid > 0 && !impl_->exited) {
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    ::waitpid(impl_->pid, &status, 0);
  }
  if (impl_ && impl_->stdin_write >= 0) {
    ::close(impl_->stdin_write);
  }
  if (impl_ && impl_->stdout_read >= 0) {
    ::close(impl_->stdout_read);
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}
ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  impl_ = std::move(other.impl_);
  return *this;
}

ChildProcess ChildProcess::spawn(const ProcessOptions& options) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
    throw TestFailure("pipe failed");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw TestFailure("fork failed");
  }
  if (pid == 0) {
    ::dup2(stdin_pipe[0], 0);
    ::dup2(stdout_pipe[1], 1);
    ::dup2(stdout_pipe[1], 2);
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    if (!options.working_directory.empty()) {
      (void)::chdir(options.working_directory.string().c_str());
    }
    const std::string executable = options.executable.string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : options.arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(stdin_pipe[0]);
  ::close(stdout_pipe[1]);
  ChildProcess child;
  child.impl_ = std::make_unique<Impl>();
  child.impl_->pid = pid;
  child.impl_->stdin_write = stdin_pipe[1];
  child.impl_->stdout_read = stdout_pipe[0];
  return child;
}

bool ChildProcess::running() { return !exited(); }

std::uint32_t ChildProcess::id() const noexcept {
  return impl_ ? static_cast<std::uint32_t>(impl_->pid) : 0;
}

bool ChildProcess::exited() {
  if (!impl_ || impl_->exited) {
    return true;
  }
  int status = 0;
  const pid_t result = ::waitpid(impl_->pid, &status, WNOHANG);
  if (result == impl_->pid) {
    impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    impl_->exited = true;
    return true;
  }
  return false;
}

bool ChildProcess::wait_for_line(const std::string& prefix, std::string& line) {
  if (!impl_) {
    return false;
  }
  for (;;) {
    const std::size_t newline = impl_->buffer.find('\n');
    if (newline != std::string::npos) {
      std::string candidate = impl_->buffer.substr(0, newline);
      impl_->buffer.erase(0, newline + 1);
      impl_->output += candidate;
      impl_->output += '\n';
      if (candidate.rfind(prefix, 0) == 0) {
        line = candidate;
        return true;
      }
      continue;
    }
    if (impl_->stdout_closed) {
      return false;
    }
    char buffer[512];
    const ssize_t read = ::read(impl_->stdout_read, buffer, sizeof(buffer));
    if (read <= 0) {
      impl_->stdout_closed = true;
      return false;
    }
    impl_->buffer.append(buffer, static_cast<std::size_t>(read));
  }
}

std::string ChildProcess::drain_available() { return {}; }

void ChildProcess::kill_hard() {
  if (!impl_ || impl_->pid <= 0 || impl_->exited) {
    return;
  }
  ::kill(impl_->pid, SIGKILL);
  int status = 0;
  ::waitpid(impl_->pid, &status, 0);
  impl_->exit_code = -1;
  impl_->exited = true;
}

void ChildProcess::close_stdin() {
  if (impl_ && impl_->stdin_write >= 0) {
    ::close(impl_->stdin_write);
    impl_->stdin_write = -1;
  }
}

std::string ChildProcess::output() const { return impl_ ? impl_->output : std::string{}; }

int ChildProcess::wait() {
  if (!impl_ || impl_->exited) {
    return impl_ ? impl_->exit_code : -1;
  }
  int status = 0;
  ::waitpid(impl_->pid, &status, 0);
  impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  impl_->exited = true;
  return impl_->exit_code;
}

bool wait_for_file(const std::filesystem::path& path) {
  for (;;) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    if (error) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool wait_for_file_contains(const std::filesystem::path& path, const std::string& marker) {
  for (;;) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      std::ifstream stream(path, std::ios::binary);
      std::ostringstream contents;
      contents << stream.rdbuf();
      if (contents.str().find(marker) != std::string::npos) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

#endif

}  // namespace fcfn::test
