// FCFN - coordinator client.
//
// The client is an ordinary session: it performs the token handshake, binds the
// session identity and coordinator epoch it observed, and stamps every request
// with a monotone sequence. It never invents authority and never assumes that a
// request was applied.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_NET_CLIENT_HPP
#define FCFN_NET_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "fcfn/core/result.hpp"
#include "fcfn/net/frame.hpp"

namespace fcfn::net {

struct ClientConfig {
  std::uint16_t port{0};
  std::string token{};
  std::string label{"fcfn-client"};
};

class CoordinatorClient {
 public:
  ~CoordinatorClient();
  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorClient>> connect(const ClientConfig& config);

  [[nodiscard]] const HelloAckPayload& handshake() const;
  [[nodiscard]] SessionId session() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] BootIdentity boot() const;
  [[nodiscard]] Sequence last_sequence() const;

  /// Send one request and wait for its response. The response is validated
  /// against the session identity before it is returned.
  [[nodiscard]] Result<ResponsePayload> call(Operation operation, std::span<const std::byte> arguments);

  void close();

 private:
  CoordinatorClient();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fcfn::net

#endif  // FCFN_NET_CLIENT_HPP
