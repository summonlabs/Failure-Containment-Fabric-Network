// fcfn_client - a real coordinator session driven by a scenario script.
//
// The client behaves like an enforcement plane: it never assumes that a request
// was applied, it always waits for the coordinator's answer, and it reports the
// coordinator's classification verbatim. Deployment scenarios and the
// multiprocess proof suites drive this binary over real loopback sockets.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "config_text.hpp"
#include "fcfn/core/json.hpp"
#include "fcfn/engine/graph.hpp"
#include "fcfn/model/plan.hpp"
#include "fcfn/net/client.hpp"
#include "fcfn/net/messages.hpp"
#include "fcfn/version.hpp"

namespace {

using namespace fcfn;  // NOLINT(google-build-using-namespace)

void usage() {
  std::fprintf(stderr,
               "usage: fcfn_client --port <n> --token <token> [--scenario <file>] [--label <text>]\n"
               "commands (one per line, '#' starts a comment):\n"
               "  describe | topology <file> | policy <file> | evidence\n"
               "  detect <resource> <present|absent|unknown> <generation>[ digest=<hex32>]\n"
               "  authorize | plan | transition <expand|contract|release>\n"
               "  apply | ack [accepted|rejected] | verify <applied|released|not_applied|partially_applied|unknown>\n"
               "  boundary | attempts | checkpoint | shutdown | expect <status-token>\n");
}

struct Session {
  std::unique_ptr<net::CoordinatorClient> client{};
  model::AuthorizationId authorization{};
  model::ContainmentPlan plan{};
  bool have_plan{false};
  AttemptSequence attempt{};
  bool have_attempt{false};
  BootIdentity client_boot{};
  ApplierEpoch applier_epoch{1};
  Sequence applier_sequence{0};
  std::string last_status{"ok"};
  /// A step failed and has not yet been claimed by an "expect" assertion.
  bool pending_failure{false};
  bool expectation_failed{false};
  std::size_t steps{0};
  std::size_t failures{0};
  int exit_code{0};
};

void emit_step(Session& session, const char* verb, const std::string& status, const std::string& json) {
  ++session.steps;
  session.last_status = status;
  if (status != "ok") {
    ++session.failures;
  }
  std::printf("STEP %zu %s status=%s json=%s\n", session.steps, verb, status.c_str(), json.c_str());
  std::fflush(stdout);
}

std::string status_json(const Status& status) {
  JsonWriter writer;
  writer.begin_object();
  writer.member("code", to_string(status.code()));
  writer.member("message", status.message());
  writer.end_object();
  return writer.take();
}

std::string response_json(const net::ResponsePayload& response) {
  if (!response.json.empty()) {
    return response.json;
  }
  return status_json(Status{response.code, response.message});
}

/// Read a string-valued field from a deterministic JSON document.
std::string json_string_field(const std::string& document, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t position = document.find(needle);
  if (position == std::string::npos) {
    return {};
  }
  const std::size_t start = position + needle.size();
  const std::size_t end = document.find('"', start);
  if (end == std::string::npos) {
    return {};
  }
  return document.substr(start, end - start);
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> parts;
  std::istringstream stream(line);
  std::string token;
  while (stream >> token) {
    parts.push_back(std::move(token));
  }
  return parts;
}

bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10ull + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

/// Run one operation and emit its classified result.
bool run_operation(Session& session, const char* verb, net::Operation operation,
                   std::span<const std::byte> arguments) {
  auto response = session.client->call(operation, arguments);
  if (!response.ok()) {
    emit_step(session, verb, to_string(response.status().code()), status_json(response.status()));
    session.pending_failure = true;
    return false;
  }
  emit_step(session, verb, to_string(response.value().code), response_json(response.value()));
  if (!is_failure(response.value().code)) {
    return true;
  }
  session.pending_failure = true;
  return false;
}

bool command_topology(Session& session, const std::vector<std::string>& parts) {
  if (parts.size() != 2) {
    emit_step(session, "topology", "invalid_argument", "{\"message\":\"topology requires a file\"}");
    session.pending_failure = true;
    return false;
  }
  auto spec = apps::parse_topology_file(parts[1]);
  if (!spec.ok()) {
    emit_step(session, "topology", to_string(spec.status().code()), status_json(spec.status()));
    session.pending_failure = true;
    return false;
  }
  auto topology = model::Topology::build(std::move(spec.value()));
  if (!topology.ok()) {
    emit_step(session, "topology", to_string(topology.status().code()), status_json(topology.status()));
    session.pending_failure = true;
    return false;
  }
  const std::vector<std::byte> encoded = topology.value().encode();
  return run_operation(session, "topology", net::Operation::ApplyTopology, encoded);
}

bool command_policy(Session& session, const std::vector<std::string>& parts) {
  if (parts.size() != 2) {
    emit_step(session, "policy", "invalid_argument", "{\"message\":\"policy requires a file\"}");
    session.pending_failure = true;
    return false;
  }
  auto policy = apps::parse_policy_file(parts[1]);
  if (!policy.ok()) {
    emit_step(session, "policy", to_string(policy.status().code()), status_json(policy.status()));
    session.pending_failure = true;
    return false;
  }
  const std::vector<std::byte> encoded = policy.value().encode();
  return run_operation(session, "policy", net::Operation::ApplyPolicy, encoded);
}

bool command_detect(Session& session, const std::vector<std::string>& parts) {
  if (parts.size() < 4) {
    emit_step(session, "detect", "invalid_argument",
              "{\"message\":\"detect requires resource, state, generation\"}");
    session.pending_failure = true;
    return false;
  }
  auto resource = model::ResourceId::parse(parts[1]);
  if (!resource.ok()) {
    emit_step(session, "detect", to_string(resource.status().code()), status_json(resource.status()));
    session.pending_failure = true;
    return false;
  }
  model::EvidenceState state{};
  if (!model::parse_evidence_state(parts[2], state)) {
    emit_step(session, "detect", "invalid_argument", "{\"message\":\"unknown evidence state\"}");
    session.pending_failure = true;
    return false;
  }
  std::uint64_t generation = 0;
  if (!parse_u64(parts[3], generation) || generation == 0) {
    emit_step(session, "detect", "invalid_argument", "{\"message\":\"generation must be positive\"}");
    session.pending_failure = true;
    return false;
  }
  model::FailureObservation observation;
  observation.resource = std::move(resource.value());
  observation.state = state;
  observation.generation = model::EvidenceGeneration{generation};
  for (std::size_t i = 4; i < parts.size(); ++i) {
    const std::size_t equals = parts[i].find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = parts[i].substr(0, equals);
    const std::string value = parts[i].substr(equals + 1);
    if (key == "digest") {
      Digest digest;
      if (!Digest::parse(value, digest)) {
        emit_step(session, "detect", "invalid_argument", "{\"message\":\"malformed digest\"}");
        session.pending_failure = true;
        return false;
      }
      observation.source_digest = digest;
    }
  }
  const std::vector<std::byte> encoded = net::encode_detection_arguments(observation);
  return run_operation(session, "detect", net::Operation::RecordDetection, encoded);
}

bool command_authorize(Session& session) {
  auto response = session.client->call(net::Operation::Authorize, {});
  if (!response.ok()) {
    emit_step(session, "authorize", to_string(response.status().code()), status_json(response.status()));
    session.pending_failure = true;
    return false;
  }
  const std::string json = response_json(response.value());
  if (is_failure(response.value().code)) {
    emit_step(session, "authorize", to_string(response.value().code), json);
    session.pending_failure = true;
    return false;
  }
  std::uint64_t identifier = 0;
  if (!apps::json_field_u64(json, "authorization_id", identifier)) {
    emit_step(session, "authorize", "protocol_violation",
              "{\"message\":\"authorization response has no identifier\"}");
    session.pending_failure = true;
    return false;
  }
  session.authorization = model::AuthorizationId{identifier};
  emit_step(session, "authorize", "ok", json);
  return true;
}

bool command_plan(Session& session) {
  auto response = session.client->call(net::Operation::Plan,
                                       net::encode_authorization_reference(session.authorization));
  if (!response.ok()) {
    emit_step(session, "plan", to_string(response.status().code()), status_json(response.status()));
    session.pending_failure = true;
    return false;
  }
  const std::string json = response_json(response.value());
  if (is_failure(response.value().code)) {
    emit_step(session, "plan", to_string(response.value().code), json);
    session.pending_failure = true;
    return false;
  }
  auto decoded = model::ContainmentPlan::decode(
      std::span<const std::byte>(response.value().result.data(), response.value().result.size()));
  if (!decoded.ok()) {
    emit_step(session, "plan", to_string(decoded.status().code()), status_json(decoded.status()));
    session.pending_failure = true;
    return false;
  }
  session.plan = std::move(decoded.value());
  session.have_plan = true;
  // The step status is the containment claim: proven_containment, indeterminate,
  // proven_infeasible, proven_feasible_not_minimal, invalid or unsupported.
  const std::string claim = json_string_field(json, "claim");
  emit_step(session, "plan", claim.empty() ? "ok" : claim, json);
  return true;
}

bool command_transition(Session& session, const std::vector<std::string>& parts) {
  if (parts.size() != 2) {
    emit_step(session, "transition", "invalid_argument", "{\"message\":\"transition requires a kind\"}");
    session.pending_failure = true;
    return false;
  }
  model::TransitionKind kind{};
  if (parts[1] == "expand") {
    kind = model::TransitionKind::Expand;
  } else if (parts[1] == "contract") {
    kind = model::TransitionKind::Contract;
  } else if (parts[1] == "release") {
    kind = model::TransitionKind::Release;
  } else {
    emit_step(session, "transition", "invalid_argument", "{\"message\":\"unknown transition kind\"}");
    session.pending_failure = true;
    return false;
  }
  if (!session.have_plan) {
    emit_step(session, "transition", "invalid_argument", "{\"message\":\"no plan has been computed\"}");
    session.pending_failure = true;
    return false;
  }
  auto boundary = session.client->call(net::Operation::CurrentBoundary, {});
  std::uint64_t current_generation = 0;
  bool released = false;
  if (boundary.ok() && !is_failure(boundary.value().code)) {
    released = boundary.value().json.find("\"generation\"") == std::string::npos;
    (void)apps::json_field_u64(boundary.value().json, "generation", current_generation);
  } else {
    released = true;
  }
  model::TransitionRequest request;
  request.kind = kind;
  request.plan = session.plan.generation;
  request.plan_digest = session.plan.digest();
  request.expected_current_boundary = model::BoundaryGeneration{current_generation};
  request.expected_released = released;
  request.authorization = session.authorization;
  const std::vector<std::byte> encoded = net::encode_transition_arguments(request);
  auto response = session.client->call(net::Operation::Transition, encoded);
  if (!response.ok()) {
    emit_step(session, "transition", to_string(response.status().code()), status_json(response.status()));
    session.pending_failure = true;
    return false;
  }
  const std::string json = response_json(response.value());
  // The step status is the decision status: accepted, denied, indeterminate,
  // stale or invalid. A recorded refusal is not a protocol failure.
  const std::string decision = json_string_field(json, "status");
  emit_step(session, "transition", decision.empty() ? to_string(response.value().code) : decision, json);
  return decision == "accepted";
}

bool command_apply(Session& session) {
  if (!session.have_plan) {
    emit_step(session, "apply", "invalid_argument", "{\"message\":\"no plan has been computed\"}");
    session.pending_failure = true;
    return false;
  }
  runtime::SubmitApplyRequest request;
  request.authorization = session.authorization;
  request.plan = session.plan.generation;
  request.plan_digest = session.plan.digest();
  const std::vector<std::byte> encoded = net::encode_submit_apply_arguments(request);
  auto response = session.client->call(net::Operation::SubmitApply, encoded);
  if (!response.ok()) {
    emit_step(session, "apply", to_string(response.status().code()), status_json(response.status()));
    session.pending_failure = true;
    return false;
  }
  const std::string json = response_json(response.value());
  if (is_failure(response.value().code)) {
    emit_step(session, "apply", to_string(response.value().code), json);
    session.pending_failure = true;
    return false;
  }
  std::uint64_t sequence = 0;
  if (apps::json_field_u64(json, "attempt_sequence", sequence)) {
    session.attempt = AttemptSequence{sequence};
    session.have_attempt = true;
    session.applier_sequence = Sequence{0};
  }
  emit_step(session, "apply", "ok", json);
  return true;
}

bool command_ack(Session& session, const std::vector<std::string>& parts) {
  const bool accepted = parts.size() < 2 || parts[1] == "accepted";
  if (!session.have_attempt) {
    emit_step(session, "ack", "invalid_argument", "{\"message\":\"no attempt is in flight\"}");
    session.pending_failure = true;
    return false;
  }
  model::ApplyAcknowledgement ack;
  ack.id.plan = session.plan.generation;
  ack.id.sequence = session.attempt;
  ack.expected_epoch = session.client->epoch();
  ack.expected_boot = session.client->boot();
  ack.applier_epoch = session.applier_epoch;
  ack.applier_boot = session.client_boot;
  session.applier_sequence = Sequence{session.applier_sequence.value() + 1};
  ack.applier_sequence = session.applier_sequence;
  ack.observed_boundary_digest = session.plan.boundary.digest();
  ack.accepted = accepted;
  const std::vector<std::byte> encoded = net::encode_acknowledgement_arguments(ack);
  return run_operation(session, "ack", net::Operation::Acknowledge, encoded);
}

bool command_verify(Session& session, const std::vector<std::string>& parts) {
  if (parts.size() != 2) {
    emit_step(session, "verify", "invalid_argument", "{\"message\":\"verify requires an observation\"}");
    session.pending_failure = true;
    return false;
  }
  if (!session.have_attempt) {
    emit_step(session, "verify", "invalid_argument", "{\"message\":\"no attempt is in flight\"}");
    session.pending_failure = true;
    return false;
  }
  model::EffectObservation observation{};
  if (!model::parse_effect_observation(parts[1], observation)) {
    emit_step(session, "verify", "invalid_argument", "{\"message\":\"unknown effect observation\"}");
    session.pending_failure = true;
    return false;
  }
  model::EffectVerification report;
  report.id.plan = session.plan.generation;
  report.id.sequence = session.attempt;
  report.expected_epoch = session.client->epoch();
  report.expected_boot = session.client->boot();
  report.applier_epoch = session.applier_epoch;
  report.applier_boot = session.client_boot;
  session.applier_sequence = Sequence{session.applier_sequence.value() + 1};
  report.applier_sequence = session.applier_sequence;
  report.observed_boundary_digest = session.plan.boundary.digest();
  report.observation = observation;
  const std::vector<std::byte> encoded = net::encode_verification_arguments(report);
  return run_operation(session, "verify", net::Operation::VerifyEffect, encoded);
}

bool run_command(Session& session, const std::vector<std::string>& parts) {
  const std::string& verb = parts[0];
  if (verb == "describe") {
    return run_operation(session, "describe", net::Operation::Describe, {});
  }
  if (verb == "topology") {
    return command_topology(session, parts);
  }
  if (verb == "policy") {
    return command_policy(session, parts);
  }
  if (verb == "evidence") {
    return run_operation(session, "evidence", net::Operation::Evidence, {});
  }
  if (verb == "detect") {
    return command_detect(session, parts);
  }
  if (verb == "authorize") {
    return command_authorize(session);
  }
  if (verb == "plan") {
    return command_plan(session);
  }
  if (verb == "transition") {
    return command_transition(session, parts);
  }
  if (verb == "apply") {
    return command_apply(session);
  }
  if (verb == "ack") {
    return command_ack(session, parts);
  }
  if (verb == "verify") {
    return command_verify(session, parts);
  }
  if (verb == "boundary") {
    return run_operation(session, "boundary", net::Operation::CurrentBoundary, {});
  }
  if (verb == "attempts") {
    return run_operation(session, "attempts", net::Operation::Attempts, {});
  }
  if (verb == "checkpoint") {
    return run_operation(session, "checkpoint", net::Operation::Checkpoint, {});
  }
  if (verb == "shutdown") {
    return run_operation(session, "shutdown", net::Operation::Shutdown, {});
  }
  if (verb == "expect") {
    if (parts.size() != 2) {
      emit_step(session, "expect", "invalid_argument", "{\"message\":\"expect requires a token\"}");
      session.pending_failure = true;
      return false;
    }
    const bool matched = session.last_status == parts[1];
    if (matched) {
      // The preceding failure was expected and no longer counts against the run.
      session.pending_failure = false;
    } else {
      ++session.failures;
      session.expectation_failed = true;
    }
    JsonWriter writer;
    writer.begin_object();
    writer.member("expected", parts[1]);
    writer.member("observed", session.last_status);
    writer.member("matched", matched);
    writer.end_object();
    ++session.steps;
    std::printf("STEP %zu expect status=%s json=%s\n", session.steps,
                matched ? "ok" : "assertion_failed", writer.take().c_str());
    std::fflush(stdout);
    return matched;
  }
  emit_step(session, verb.c_str(), "invalid_argument", "{\"message\":\"unknown command\"}");
  session.pending_failure = true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  net::ClientConfig config;
  std::string scenario_path;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (flag == "--port") {
      config.port = static_cast<std::uint16_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (flag == "--token") {
      config.token = next();
    } else if (flag == "--label") {
      config.label = next();
    } else if (flag == "--scenario") {
      scenario_path = next();
    } else {
      usage();
      return 2;
    }
  }
  if (config.port == 0 || config.token.empty()) {
    usage();
    return 2;
  }

  auto client = net::CoordinatorClient::connect(config);
  if (!client.ok()) {
    std::fprintf(stderr, "fcfn_client: %s\n", client.status().to_string().c_str());
    return 1;
  }

  Session session;
  session.client = std::move(client.value());
  session.client_boot = BootIdentity{BootId{static_cast<std::uint64_t>(session.client->session().value()) + 0x1000},
                                     0};

  std::istream* input = &std::cin;
  std::ifstream file;
  if (!scenario_path.empty()) {
    file.open(scenario_path);
    if (!file) {
      std::fprintf(stderr, "fcfn_client: cannot open scenario file\n");
      return 1;
    }
    input = &file;
  }

  std::printf("HANDSHAKE session=%llu epoch=%llu boot=%016llx/%lu version=%s\n",
              static_cast<unsigned long long>(session.client->session().value()),
              static_cast<unsigned long long>(session.client->epoch().value()),
              static_cast<unsigned long long>(session.client->boot().id.value()),
              static_cast<unsigned long>(session.client->boot().process_id),
              session.client->handshake().version.c_str());
  std::fflush(stdout);

  std::string line;
  while (std::getline(*input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    const std::vector<std::string> parts = split(line);
    if (parts.empty()) {
      continue;
    }
    if (!run_command(session, parts)) {
      // Commands keep running so a scenario can assert the failure and continue.
      continue;
    }
  }

  if (session.pending_failure || session.expectation_failed) {
    session.exit_code = 1;
  }
  std::printf("SUMMARY steps=%zu failures=%zu exit=%d\n", session.steps, session.failures,
              session.exit_code);
  std::fflush(stdout);
  session.client->close();
  return session.exit_code;
}
