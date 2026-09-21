// FCFN - coordinator service.
//
// One process, one runtime incarnation, one listening socket on the loopback
// interface. Each accepted connection performs a token handshake, is bound to a
// session identity, and may only act under the authority bound to that session
// and to the coordinator epoch advertised at handshake time.
//
// Ownership and locking rules (see docs/CONCURRENCY_AUDIT.md):
//   * session threads never hold the session table lock while calling the
//     runtime, and never hold the runtime lock while touching the session table
//   * socket teardown closes the socket handle first so blocked reads return
//   * shutdown joins threads only after the listener and every socket are closed
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_NET_SERVER_HPP
#define FCFN_NET_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "fcfn/core/result.hpp"
#include "fcfn/net/socket.hpp"
#include "fcfn/runtime/coordinator.hpp"

namespace fcfn::net {

struct ServerConfig {
  runtime::RuntimeConfig runtime{};
  std::string session_token{};
  std::uint16_t port{0};
  std::size_t max_sessions{kMaxSessions};
  /// Optional induced crash point for multiprocess fencing proofs. Recognised
  /// values: "before_commit", "after_commit_before_ack", "after_ack", "".
  std::string crash_point{};
  /// 1-based ordinal of the mutating request at which the crash point triggers.
  std::uint64_t crash_after{0};
};

class CoordinatorServer {
 public:
  /// Implementation state (defined in server.cpp).
  struct Impl;

  ~CoordinatorServer();
  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorServer>> start(const ServerConfig& config);

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] runtime::ContainmentRuntime& runtime();
  [[nodiscard]] std::size_t session_count() const;

  /// Accept loop. Returns once shutdown has been requested and every session
  /// has been joined. Never uses a timeout.
  [[nodiscard]] VoidResult serve();

  /// Release the listener and every session so serve() and session threads end.
  void request_shutdown();

 private:
  CoordinatorServer();
  std::unique_ptr<Impl> impl_;
};

}  // namespace fcfn::net

#endif  // FCFN_NET_SERVER_HPP
