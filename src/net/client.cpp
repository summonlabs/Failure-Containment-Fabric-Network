// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/net/client.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "fcfn/net/socket.hpp"

namespace fcfn::net {
namespace {

constexpr std::size_t kReadChunkBytes = 4096;

}  // namespace

struct CoordinatorClient::Impl {
  std::unique_ptr<Socket> socket{};
  FrameStream stream{};
  HelloAckPayload handshake{};
  Sequence sequence{0};
};

CoordinatorClient::CoordinatorClient() = default;
CoordinatorClient::~CoordinatorClient() { close(); }

Result<std::unique_ptr<CoordinatorClient>> CoordinatorClient::connect(const ClientConfig& config) {
  if (config.port == 0) {
    return Status{StatusCode::InvalidArgument, "client requires a port"};
  }
  if (config.token.empty()) {
    return Status{StatusCode::InvalidArgument, "client requires a session token"};
  }
  auto client = std::unique_ptr<CoordinatorClient>(new CoordinatorClient());
  client->impl_ = std::make_unique<Impl>();

  auto socket = Socket::connect_loopback(config.port);
  if (!socket.ok()) {
    return socket.status();
  }
  client->impl_->socket = std::move(socket.value());

  HelloPayload hello;
  hello.token = config.token;
  hello.label = config.label;
  Frame frame;
  frame.type = MessageType::Hello;
  frame.payload = encode_hello(hello);
  const std::vector<std::byte> encoded = encode_frame(frame);
  const VoidResult sent = client->impl_->socket->write_all(encoded);
  if (!sent.ok()) {
    return sent.status();
  }

  std::vector<std::byte> buffer(kReadChunkBytes);
  for (;;) {
    const DecodeOutcome outcome = client->impl_->stream.next();
    if (outcome.status == FrameStatus::NeedMore) {
      const Result<std::size_t> read = client->impl_->socket->read(buffer);
      if (!read.ok() || read.value() == 0) {
        return Status{StatusCode::IoError, "coordinator closed the connection during handshake"};
      }
      if (!client->impl_->stream.feed(std::span<const std::byte>(buffer.data(), read.value()))) {
        return Status{StatusCode::ProtocolViolation, "handshake exceeded the frame bound"};
      }
      continue;
    }
    if (outcome.status != FrameStatus::Complete) {
      return Status{StatusCode::ProtocolViolation, to_string(outcome.status)};
    }
    if (outcome.frame.type != MessageType::HelloAck) {
      return Status{StatusCode::ProtocolViolation, "coordinator did not answer the handshake"};
    }
    auto ack = decode_hello_ack(outcome.frame.payload);
    if (!ack.ok()) {
      return ack.status();
    }
    if (!ack.value().accepted) {
      client->impl_->socket->shutdown();
      return Status{StatusCode::Unauthorized, ack.value().reason};
    }
    client->impl_->handshake = ack.value();
    return client;
  }
}

const HelloAckPayload& CoordinatorClient::handshake() const { return impl_->handshake; }

SessionId CoordinatorClient::session() const { return impl_->handshake.session; }

CoordinatorEpoch CoordinatorClient::epoch() const { return impl_->handshake.epoch; }

BootIdentity CoordinatorClient::boot() const { return impl_->handshake.boot; }

Sequence CoordinatorClient::last_sequence() const { return impl_->sequence; }

Result<ResponsePayload> CoordinatorClient::call(Operation operation,
                                                std::span<const std::byte> arguments) {
  RequestPayload request;
  request.operation = operation;
  request.arguments.assign(arguments.begin(), arguments.end());

  impl_->sequence = Sequence{impl_->sequence.value() + 1};
  Frame frame;
  frame.type = MessageType::Request;
  frame.session = impl_->handshake.session;
  frame.sequence = impl_->sequence;
  frame.epoch = impl_->handshake.epoch;
  frame.payload = encode_request(request);

  const std::vector<std::byte> encoded = encode_frame(frame);
  const VoidResult sent = impl_->socket->write_all(encoded);
  if (!sent.ok()) {
    return sent.status();
  }

  std::vector<std::byte> buffer(kReadChunkBytes);
  for (;;) {
    const DecodeOutcome outcome = impl_->stream.next();
    if (outcome.status == FrameStatus::NeedMore) {
      const Result<std::size_t> read = impl_->socket->read(buffer);
      if (!read.ok() || read.value() == 0) {
        return Status{StatusCode::IoError, "coordinator closed the connection"};
      }
      if (!impl_->stream.feed(std::span<const std::byte>(buffer.data(), read.value()))) {
        return Status{StatusCode::ProtocolViolation, "response exceeded the frame bound"};
      }
      continue;
    }
    if (outcome.status != FrameStatus::Complete) {
      return Status{StatusCode::ProtocolViolation, to_string(outcome.status)};
    }
    if (outcome.frame.type != MessageType::Response) {
      return Status{StatusCode::ProtocolViolation, "coordinator sent an unexpected message type"};
    }
    if (!(outcome.frame.session == impl_->handshake.session)) {
      return Status{StatusCode::Unauthorized, "response belongs to another session"};
    }
    if (!(outcome.frame.sequence == impl_->sequence)) {
      return Status{StatusCode::SequenceRegression, "response sequence does not match the request"};
    }
    auto response = decode_response(outcome.frame.payload);
    if (!response.ok()) {
      return response.status();
    }
    return response.value();
  }
}

void CoordinatorClient::close() {
  if (impl_ != nullptr && impl_->socket != nullptr) {
    impl_->socket->shutdown();
    impl_->socket.reset();
  }
}

}  // namespace fcfn::net
