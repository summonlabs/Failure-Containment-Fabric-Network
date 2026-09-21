// FCFN - portable blocking TCP over loopback.
//
// The runtime uses plain blocking sockets with explicit shutdown: closing a
// socket from another thread releases a thread blocked in accept() or read().
// That is how server shutdown terminates its sessions without timeouts.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_NET_SOCKET_HPP
#define FCFN_NET_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "fcfn/core/result.hpp"

namespace fcfn::net {

/// Initialise the platform socket layer once per process (WSAStartup on Windows).
[[nodiscard]] VoidResult ensure_socket_layer();

class Socket;

/// Listening socket bound to the loopback interface.
class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  /// Bind to 127.0.0.1 on the requested port (0 selects an ephemeral port).
  [[nodiscard]] static Result<Listener> bind_loopback(std::uint16_t port);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept;

  /// Accept one connection. Returns a closed/invalid status after shutdown().
  [[nodiscard]] Result<std::unique_ptr<Socket>> accept();

  /// Release a blocked accept() and refuse further connections.
  void shutdown();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::uint16_t port_{0};
};

/// Connected socket. Never throws; every operation reports a Status.
class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  /// Read up to bytes.size() bytes. Zero means the peer closed.
  [[nodiscard]] Result<std::size_t> read(std::span<std::byte> bytes);

  /// Write everything or fail.
  [[nodiscard]] VoidResult write_all(std::span<const std::byte> bytes);

  /// Release blocked reads/writes in other threads.
  void shutdown();

  /// Connect to a loopback listener.
  [[nodiscard]] static Result<std::unique_ptr<Socket>> connect_loopback(std::uint16_t port);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string peer() const;

 private:
  friend class Listener;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace fcfn::net

#endif  // FCFN_NET_SOCKET_HPP
