// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/net/server.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/core/json.hpp"
#include "fcfn/net/frame.hpp"
#include "fcfn/net/messages.hpp"
#include "fcfn/version.hpp"

namespace fcfn::net {
namespace {

constexpr std::size_t kReadChunkBytes = 4096;

void print_crash_point(const char* point, std::uint64_t ordinal) {
  std::printf("CRASH-POINT %s %llu\n", point, static_cast<unsigned long long>(ordinal));
  std::fflush(stdout);
  // Block until the supervising process kills this process or closes stdin. No
  // timeout: the multiprocess proof kills the process deterministically.
  char buffer[1];
  const std::size_t read = std::fread(buffer, 1, 1, stdin);
  (void)read;
  std::printf("CRASH-POINT-MISSED %s\n", point);
  std::fflush(stdout);
  std::exit(97);
}

}  // namespace

struct CoordinatorServer::Impl {
  struct SessionThread {
    std::thread thread{};
    std::shared_ptr<std::atomic<bool>> finished{};
  };

  ServerConfig config{};
  SteadyClock runtime_clock{};
  std::unique_ptr<runtime::ContainmentRuntime> runtime{};
  Listener listener{};
  std::atomic<bool> shutting_down{false};
  std::atomic<std::uint64_t> session_counter{0};
  std::atomic<std::uint64_t> mutation_counter{0};
  mutable std::mutex sessions_lock{};
  mutable std::mutex threads_lock{};
  std::vector<SessionThread> threads{};
  std::vector<std::shared_ptr<Socket>> sockets{};

  /// Join threads that have already finished. Never called while holding
  /// sessions_lock, and never joins the calling thread.
  void reap_finished_threads() {
    const std::thread::id self = std::this_thread::get_id();
    std::vector<std::thread> ready;
    {
      const std::lock_guard<std::mutex> guard(threads_lock);
      for (auto it = threads.begin(); it != threads.end();) {
        if (it->finished != nullptr && it->finished->load() && it->thread.get_id() != self) {
          ready.push_back(std::move(it->thread));
          it = threads.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (std::thread& thread : ready) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }
};

CoordinatorServer::CoordinatorServer() = default;

CoordinatorServer::~CoordinatorServer() { request_shutdown(); }

Result<std::unique_ptr<CoordinatorServer>> CoordinatorServer::start(const ServerConfig& config) {
  if (config.session_token.empty()) {
    return Status{StatusCode::InvalidArgument, "server requires a session token"};
  }
  if (config.max_sessions == 0 || config.max_sessions > kMaxSessions) {
    return Status{StatusCode::InvalidArgument, "session bound out of range"};
  }
  if (!config.crash_point.empty()) {
    if (config.crash_point != "before_commit" && config.crash_point != "after_commit_before_ack" &&
        config.crash_point != "after_ack") {
      return Status{StatusCode::InvalidArgument, "unrecognised crash point"};
    }
    if (config.crash_after == 0) {
      return Status{StatusCode::InvalidArgument, "crash point requires a positive ordinal"};
    }
  }

  const VoidResult initialized = ensure_socket_layer();
  if (!initialized.ok()) {
    return initialized.status();
  }
  auto server = std::unique_ptr<CoordinatorServer>(new CoordinatorServer());
  server->impl_ = std::make_unique<Impl>();
  server->impl_->config = config;

  auto runtime = runtime::ContainmentRuntime::open(config.runtime, &server->impl_->runtime_clock);
  if (!runtime.ok()) {
    return runtime.status();
  }
  server->impl_->runtime = std::move(runtime.value());

  auto listener = Listener::bind_loopback(config.port);
  if (!listener.ok()) {
    return listener.status();
  }
  server->impl_->listener = std::move(listener.value());
  return server;
}

std::uint16_t CoordinatorServer::port() const noexcept { return impl_->listener.port(); }

runtime::ContainmentRuntime& CoordinatorServer::runtime() { return *impl_->runtime; }

std::size_t CoordinatorServer::session_count() const {
  const std::lock_guard<std::mutex> guard(impl_->sessions_lock);
  return impl_->sockets.size();
}

void CoordinatorServer::request_shutdown() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->shutting_down.store(true);
  // Closing the listener and every socket releases blocked reads and accepts.
  impl_->listener.shutdown();
  std::vector<std::shared_ptr<Socket>> sockets;
  {
    const std::lock_guard<std::mutex> guard(impl_->sessions_lock);
    sockets = impl_->sockets;
  }
  for (const std::shared_ptr<Socket>& socket : sockets) {
    socket->shutdown();
  }

  // Threads are joined outside every lock, and never by themselves.
  const std::thread::id self = std::this_thread::get_id();
  std::vector<std::thread> pending;
  {
    const std::lock_guard<std::mutex> guard(impl_->threads_lock);
    for (Impl::SessionThread& entry : impl_->threads) {
      if (entry.thread.get_id() == self) {
        if (entry.thread.joinable()) {
          entry.thread.detach();
        }
        continue;
      }
      pending.push_back(std::move(entry.thread));
    }
    impl_->threads.clear();
  }
  for (std::thread& thread : pending) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  const std::lock_guard<std::mutex> guard(impl_->sessions_lock);
  impl_->sockets.clear();
}

namespace {

using namespace fcfn;  // NOLINT(google-build-using-namespace)

/// Build the response for one request. Returns the response payload.
ResponsePayload dispatch(CoordinatorServer::Impl& impl, Operation operation,
                         std::span<const std::byte> arguments) {
  ResponsePayload response;
  response.operation = operation;
  runtime::ContainmentRuntime& rt = *impl.runtime;

  auto ok_with_json = [&response](std::string json) {
    response.code = StatusCode::Ok;
    response.json = std::move(json);
  };
  auto fail_with = [&response](const Status& status) {
    response.code = status.code();
    response.message = status.message();
  };

  switch (operation) {
    case Operation::Describe: {
      const model::AuthorityVector authority = rt.authority();
      JsonWriter authority_writer;
      authority_writer.begin_object();
      authority_writer.member("epoch", authority.epoch.value());
      authority_writer.member("boot_id", authority.boot.id.value());
      authority_writer.member("boot_pid", static_cast<std::uint64_t>(authority.boot.process_id));
      authority_writer.member("policy_generation", authority.policy.value());
      authority_writer.member("topology_generation", authority.topology.value());
      authority_writer.member("evidence_revision", authority.evidence_revision.value());
      authority_writer.member("issued_sequence", authority.issued_sequence.value());
      authority_writer.member("authority_digest", authority.digest().hex());
      authority_writer.end_object();
      std::string document = "{\"version\":";
      document += json_escape(kVersionString);
      document += ",\"semantic_version\":";
      document += std::to_string(kSemanticVersion);
      document += ",\"startup\":";
      document += runtime::to_json(rt.startup());
      document += ",\"authority\":";
      document += authority_writer.str();
      document += ",\"stats\":";
      document += runtime::to_json(rt.stats());
      document += "}";
      ok_with_json(document);
      break;
    }
    case Operation::ApplyTopology: {
      auto decoded = model::Topology::decode(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      model::TopologySpec spec;
      spec.generation = decoded.value().generation();
      for (std::size_t i = 0; i < decoded.value().node_count(); ++i) {
        spec.nodes.push_back(decoded.value().node(static_cast<model::NodeIndex>(i)));
      }
      for (std::size_t i = 0; i < decoded.value().edge_count(); ++i) {
        const model::Topology::Edge& edge = decoded.value().edge_by_index(static_cast<model::EdgeIndex>(i));
        model::EdgeSpec edge_spec;
        edge_spec.from = decoded.value().resource(edge.from);
        edge_spec.to = decoded.value().resource(edge.to);
        edge_spec.evidence = edge.evidence;
        edge_spec.generation = edge.generation;
        spec.edges.push_back(std::move(edge_spec));
      }
      auto applied = rt.apply_topology(std::move(spec));
      if (!applied.ok()) {
        fail_with(applied.status());
        break;
      }
      JsonWriter writer;
      writer.begin_object();
      writer.member("topology_generation", applied.value().value());
      writer.end_object();
      ok_with_json(writer.take());
      break;
    }
    case Operation::ApplyPolicy: {
      auto decoded = model::ContainmentPolicy::decode(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto applied = rt.apply_policy(decoded.value());
      if (!applied.ok()) {
        fail_with(applied.status());
        break;
      }
      JsonWriter writer;
      writer.begin_object();
      writer.member("policy_generation", applied.value().value());
      writer.end_object();
      ok_with_json(writer.take());
      break;
    }
    case Operation::RecordDetection: {
      auto decoded = decode_detection_arguments(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto recorded = rt.record_detection(decoded.value());
      if (!recorded.ok()) {
        fail_with(recorded.status());
        break;
      }
      JsonWriter writer;
      writer.begin_object();
      writer.member("resource", recorded.value().resource.value());
      writer.member("state", model::to_string(recorded.value().state));
      writer.member("generation", recorded.value().generation.value());
      writer.member("sequence", recorded.value().sequence.value());
      writer.member("record_digest", recorded.value().record_digest.hex());
      writer.end_object();
      ok_with_json(writer.take());
      break;
    }
    case Operation::Authorize: {
      auto authorization = rt.authorize();
      if (!authorization.ok()) {
        fail_with(authorization.status());
        break;
      }
      ok_with_json(runtime::to_json(authorization.value()));
      break;
    }
    case Operation::Plan: {
      auto decoded = decode_authorization_reference(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      runtime::PlanRequest request;
      request.authorization = decoded.value();
      auto planned = rt.plan(request);
      if (!planned.ok()) {
        fail_with(planned.status());
        break;
      }
      response.result = planned.value().encode();
      ok_with_json(planned.value().to_json());
      break;
    }
    case Operation::Transition: {
      auto decoded = decode_transition_arguments(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto decided = rt.transition(decoded.value());
      if (!decided.ok()) {
        fail_with(decided.status());
        break;
      }
      ok_with_json(decided.value().to_json());
      break;
    }
    case Operation::SubmitApply: {
      auto decoded = decode_submit_apply_arguments(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto submitted = rt.submit_apply(decoded.value());
      if (!submitted.ok()) {
        fail_with(submitted.status());
        break;
      }
      ok_with_json(runtime::to_json(submitted.value()));
      break;
    }
    case Operation::Acknowledge: {
      auto decoded = decode_acknowledgement_arguments(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto acknowledged = rt.acknowledge(decoded.value());
      if (!acknowledged.ok()) {
        fail_with(acknowledged.status());
        break;
      }
      ok_with_json(runtime::to_json(acknowledged.value()));
      break;
    }
    case Operation::VerifyEffect: {
      auto decoded = decode_verification_arguments(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto verified = rt.verify_effect(decoded.value());
      if (!verified.ok()) {
        fail_with(verified.status());
        break;
      }
      ok_with_json(runtime::to_json(verified.value()));
      break;
    }
    case Operation::CurrentBoundary: {
      auto boundary = rt.current_boundary();
      if (!boundary.ok()) {
        fail_with(boundary.status());
        break;
      }
      std::string document = "{\"boundary\":";
      document += boundary.value().to_json();
      document += ",\"effect_state\":";
      document += json_escape(model::to_string(rt.current_effect_state()));
      document += "}";
      ok_with_json(document);
      break;
    }
    case Operation::PlanByGeneration: {
      auto decoded = decode_plan_generation(arguments);
      if (!decoded.ok()) {
        fail_with(decoded.status());
        break;
      }
      auto stored = rt.plan_by_generation(decoded.value());
      if (!stored.ok()) {
        fail_with(stored.status());
        break;
      }
      response.result = stored.value().encode();
      ok_with_json(stored.value().to_json());
      break;
    }
    case Operation::Attempts: {
      std::string document = "[";
      bool first = true;
      for (const model::ApplyAttempt& attempt : rt.attempts()) {
        if (!first) {
          document += ",";
        }
        first = false;
        document += runtime::to_json(attempt);
      }
      document += "]";
      ok_with_json(document);
      break;
    }
    case Operation::Evidence: {
      auto evidence = rt.evidence();
      if (!evidence.ok()) {
        fail_with(evidence.status());
        break;
      }
      JsonWriter writer;
      writer.begin_array();
      for (const model::EvidenceEntry& entry : evidence.value().entries()) {
        writer.begin_object();
        writer.member("resource", entry.resource.value());
        writer.member("state", model::to_string(entry.state));
        writer.member("generation", entry.generation.value());
        writer.member("source_digest", entry.source_digest.hex());
        writer.end_object();
      }
      writer.end_array();
      ok_with_json(writer.take());
      break;
    }
    case Operation::Checkpoint: {
      const VoidResult checkpointed = rt.checkpoint();
      if (!checkpointed.ok()) {
        fail_with(checkpointed.status());
        break;
      }
      JsonWriter writer;
      writer.begin_object();
      writer.member("checkpointed", true);
      writer.member("sequence", rt.authority().issued_sequence.value());
      writer.end_object();
      ok_with_json(writer.take());
      break;
    }
    case Operation::Shutdown: {
      JsonWriter writer;
      writer.begin_object();
      writer.member("shutdown", true);
      writer.end_object();
      ok_with_json(writer.take());
      break;
    }
  }
  return response;
}

void send_response(Socket& socket, const Frame& request, const ResponsePayload& response) {
  Frame frame;
  frame.type = MessageType::Response;
  frame.session = request.session;
  frame.sequence = request.sequence;
  frame.epoch = request.epoch;
  frame.payload = encode_response(response);
  const std::vector<std::byte> bytes = encode_frame(frame);
  (void)socket.write_all(bytes);
}

void send_error(Socket& socket, const Frame& request, StatusCode code, std::string_view message) {
  ResponsePayload response;
  response.operation = Operation::Describe;
  response.code = code;
  response.message = std::string{message};
  send_response(socket, request, response);
}

/// One session thread.
void run_session(CoordinatorServer::Impl& impl, std::shared_ptr<Socket> socket) {
  FrameStream stream;
  std::vector<std::byte> buffer(kReadChunkBytes);

  auto read_more = [&]() -> bool {
    const Result<std::size_t> read = socket->read(buffer);
    if (!read.ok() || read.value() == 0) {
      return false;
    }
    return stream.feed(std::span<const std::byte>(buffer.data(), read.value()));
  };

  // ---- handshake -------------------------------------------------------
  HelloPayload hello;
  bool established = false;
  while (!established) {
    const DecodeOutcome outcome = stream.next();
    if (outcome.status == FrameStatus::NeedMore) {
      if (!read_more()) {
        socket->shutdown();
        return;
      }
      continue;
    }
    if (outcome.status != FrameStatus::Complete || outcome.frame.type != MessageType::Hello) {
      HelloAckPayload ack;
      ack.accepted = false;
      ack.reason = outcome.status == FrameStatus::Complete ? "expected hello" : to_string(outcome.status);
      Frame frame;
      frame.type = MessageType::HelloAck;
      frame.payload = encode_hello_ack(ack);
      const std::vector<std::byte> bytes = encode_frame(frame);
      (void)socket->write_all(bytes);
      socket->shutdown();
      return;
    }
    auto decoded = decode_hello(outcome.frame.payload);
    if (!decoded.ok()) {
      socket->shutdown();
      return;
    }
    hello = decoded.value();
    if (hello.token != impl.config.session_token) {
      HelloAckPayload ack;
      ack.accepted = false;
      ack.reason = "unauthorized";
      Frame frame;
      frame.type = MessageType::HelloAck;
      frame.payload = encode_hello_ack(ack);
      const std::vector<std::byte> bytes = encode_frame(frame);
      (void)socket->write_all(bytes);
      socket->shutdown();
      return;
    }
    established = true;
  }

  {
    const std::lock_guard<std::mutex> guard(impl.sessions_lock);
    if (impl.sockets.size() >= impl.config.max_sessions) {
      HelloAckPayload ack;
      ack.accepted = false;
      ack.reason = "session table full";
      Frame frame;
      frame.type = MessageType::HelloAck;
      frame.payload = encode_hello_ack(ack);
      const std::vector<std::byte> bytes = encode_frame(frame);
      (void)socket->write_all(bytes);
      socket->shutdown();
      return;
    }
  }

  const SessionId session{impl.session_counter.fetch_add(1) + 1};
  HelloAckPayload ack;
  ack.session = session;
  ack.epoch = impl.runtime->authority().epoch;
  ack.boot = impl.runtime->authority().boot;
  ack.accepted = true;
  ack.version = kVersionString;
  {
    Frame frame;
    frame.type = MessageType::HelloAck;
    frame.payload = encode_hello_ack(ack);
    const std::vector<std::byte> bytes = encode_frame(frame);
    if (!socket->write_all(bytes).ok()) {
      socket->shutdown();
      return;
    }
  }

  // ---- request loop ----------------------------------------------------
  Sequence last_sequence{0};
  for (;;) {
    if (impl.shutting_down.load()) {
      break;
    }
    const DecodeOutcome outcome = stream.next();
    if (outcome.status == FrameStatus::NeedMore) {
      if (!read_more()) {
        break;
      }
      continue;
    }
    if (outcome.status != FrameStatus::Complete) {
      // A protocol violation poisons the session: report and close.
      Frame frame;
      frame.session = session;
      frame.sequence = last_sequence;
      send_error(*socket, frame, StatusCode::ProtocolViolation, to_string(outcome.status));
      break;
    }
    const Frame& frame = outcome.frame;
    if (frame.type != MessageType::Request) {
      send_error(*socket, frame, StatusCode::ProtocolViolation, "expected a request frame");
      break;
    }
    if (!(frame.session == session)) {
      send_error(*socket, frame, StatusCode::Unauthorized, "session identity mismatch");
      break;
    }
    if (!(last_sequence < frame.sequence)) {
      send_error(*socket, frame, StatusCode::SequenceRegression, "request sequence did not advance");
      break;
    }
    last_sequence = frame.sequence;
    const CoordinatorEpoch current_epoch = impl.runtime->authority().epoch;
    if (!(frame.epoch == current_epoch)) {
      send_error(*socket, frame, StatusCode::StaleGeneration, "request carries a stale epoch");
      break;
    }
    auto request = decode_request(frame.payload);
    if (!request.ok()) {
      send_error(*socket, frame, request.status().code(), request.status().message());
      break;
    }

    const bool mutating = is_mutating_operation(request.value().operation);
    std::uint64_t ordinal = 0;
    if (mutating) {
      ordinal = impl.mutation_counter.fetch_add(1) + 1;
      if (impl.config.crash_point == "before_commit" && ordinal == impl.config.crash_after) {
        print_crash_point("before_commit", ordinal);
      }
    }

    const ResponsePayload response = dispatch(impl, request.value().operation, request.value().arguments);

    if (mutating && impl.config.crash_point == "after_commit_before_ack" &&
        ordinal == impl.config.crash_after) {
      print_crash_point("after_commit_before_ack", ordinal);
    }

    send_response(*socket, frame, response);

    if (mutating && impl.config.crash_point == "after_ack" && ordinal == impl.config.crash_after) {
      print_crash_point("after_ack", ordinal);
    }

    if (request.value().operation == Operation::Shutdown) {
      break;
    }
  }
  socket->shutdown();
}

}  // namespace

VoidResult CoordinatorServer::serve() {
  for (;;) {
    if (impl_->shutting_down.load()) {
      break;
    }
    Result<std::unique_ptr<Socket>> accepted = impl_->listener.accept();
    if (!accepted.ok()) {
      if (impl_->shutting_down.load()) {
        break;
      }
      // A released accept is the documented shutdown path; anything else is
      // reported to the caller.
      if (accepted.status().code() != StatusCode::Interrupted) {
        return accepted.status();
      }
      break;
    }
    impl_->reap_finished_threads();
    std::shared_ptr<Socket> socket(std::move(accepted.value()));
    {
      const std::lock_guard<std::mutex> guard(impl_->sessions_lock);
      impl_->sockets.push_back(socket);
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, socket, finished]() {
      run_session(*impl_, socket);
      {
        const std::lock_guard<std::mutex> guard(impl_->sessions_lock);
        const auto position = std::find(impl_->sockets.begin(), impl_->sockets.end(), socket);
        if (position != impl_->sockets.end()) {
          impl_->sockets.erase(position);
        }
      }
      finished->store(true);
    });
    {
      const std::lock_guard<std::mutex> guard(impl_->threads_lock);
      impl_->threads.push_back(Impl::SessionThread{std::move(worker), finished});
    }
  }
  request_shutdown();
  return ok_result();
}

}  // namespace fcfn::net
