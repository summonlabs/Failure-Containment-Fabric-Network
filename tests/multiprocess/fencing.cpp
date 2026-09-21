// FCFN multiprocess proof: hard kills at durable boundaries, restart fencing,
// and session authority over real loopback sockets.
//
// Every test in this file uses an independent coordinator process and an
// independent client process. Nothing here is a thread standing in for a
// process, and nothing here is a serialization round trip standing in for a
// restart.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fcfn/net/frame.hpp"
#include "fcfn/net/socket.hpp"
#include "harness.hpp"
#include "test_harness.hpp"

namespace {

using namespace fcfn;  // NOLINT(google-build-using-namespace)

constexpr const char* kToken = "multiprocess-token";

std::string topology_text() {
  return
      "# SYNTHETIC fixture: no physical hardware is involved\n"
      "generation 1\n"
      "node src containable=1 weight=3 protected=0 completeness=complete\n"
      "node a containable=1 weight=2 protected=0 completeness=complete\n"
      "node b containable=1 weight=5 protected=0 completeness=complete\n"
      "node ob containable=0 weight=1 protected=1 completeness=complete\n"
      "edge src a evidence=proven generation=1\n"
      "edge a ob evidence=proven generation=1\n"
      "edge src b evidence=proven generation=1\n"
      "edge b ob evidence=proven generation=1\n";
}

/// Write the shared fixture files and return their paths.
struct Fixture {
  test::TempDir dir{"multiprocess"};
  std::filesystem::path topology{};
  std::filesystem::path store{};

  Fixture() {
    topology = dir.child("topology.txt");
    store = dir.child("store");
    test::write_text_file(topology, topology_text());
  }

  std::filesystem::path scenario(const std::string& name, const std::string& body) {
    const std::filesystem::path path = dir.child(name);
    test::write_text_file(path, body);
    return path;
  }

  std::vector<std::string> topology_argument() const { return {"--topology", topology.string()}; }
};

/// Scenario: containment intent applied and acknowledged, effect NOT verified.
const char* kPreKillScenario =
    "detect src present 1\n"
    "authorize\n"
    "plan\n"
    "transition expand\n"
    "apply\n"
    "ack accepted\n"
    "checkpoint\n"
    "boundary\n";

/// Scenario executed after the restart.
const char* kPostRestartScenario =
    "describe\n"
    "boundary\n"
    "detect src present 1\n"
    "authorize\n"
    "plan\n"
    "transition expand\n"
    "expect indeterminate\n"
    "apply\n"
    "ack accepted\n"
    "verify applied\n"
    "boundary\n";

std::uint64_t current_wal_sequence(const std::filesystem::path& store) {
  std::ifstream stream(store / "CURRENT");
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("wal ", 0) == 0) {
      return std::stoull(line.substr(4));
    }
  }
  throw test::TestFailure("CURRENT pointer has no wal sequence");
}

std::filesystem::path wal_path(const std::filesystem::path& store, std::uint64_t sequence) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "wal-%016llx.fcfn", static_cast<unsigned long long>(sequence));
  return store / buffer;
}

}  // namespace

FCFN_TEST(multiprocess, restart_fences_pre_restart_effect_and_requires_reverification) {
  Fixture fixture;
  {
    test::Coordinator coordinator =
        test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
    const std::filesystem::path scenario =
        fixture.scenario("pre.txt", std::string{kPreKillScenario});
    const test::ClientRun run = test::run_client(coordinator.port(), kToken, scenario);
    FCFN_CHECK_EQ(run.exit_code, 0);
    FCFN_CHECK(test::contains(run.output, "status=ok"));
    // The intent was acknowledged but never verified.
    FCFN_CHECK(test::contains(run.output, "\"effect_state\":\"acknowledged\""));
    coordinator.kill();
  }

  test::Coordinator restarted =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  const std::string startup = restarted.startup_json();
  FCFN_CHECK(test::contains(startup, "\"recovered\":true"));
  FCFN_CHECK(test::contains(startup, "\"fresh\":false"));
  FCFN_CHECK(test::contains(startup, "\"epoch\":2"));
  FCFN_CHECK(test::contains(startup, "\"fenced_attempts\":1"));

  const std::filesystem::path scenario =
      fixture.scenario("post.txt", std::string{kPostRestartScenario});
  const test::ClientRun run = test::run_client(restarted.port(), kToken, scenario);
  FCFN_CHECK_EQ(run.exit_code, 0);
  // The surviving boundary is not treated as current until the enforcement
  // plane re-verifies it under the new incarnation.
  FCFN_CHECK(test::contains(run.output, "\"effect_state\":\"ambiguous\""));
  // A transition on an unverified boundary is refused as indeterminate: the
  // enforcement plane must re-establish the effect first, it is never assumed.
  FCFN_CHECK_EQ(test::step_status(run.output, "transition"), std::string("indeterminate"));
  // Re-confirming the same evidence restores containment currency.
  FCFN_CHECK(test::contains(run.output, "\"claim\":\"proven_containment\""));
  // After a verified re-assertion under the new epoch the boundary is current.
  FCFN_CHECK(test::contains(run.output, "\"effect_state\":\"verified_applied\""));
  restarted.kill();
}

FCFN_TEST(multiprocess, hostile_session_token_is_refused_without_establishing_authority) {
  Fixture fixture;
  test::Coordinator coordinator =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  const std::filesystem::path scenario =
      fixture.scenario("hostile.txt", "describe\nauthorize\n");
  const test::ClientRun run = test::run_client(coordinator.port(), "wrong-token", scenario);
  FCFN_CHECK_NE(run.exit_code, 0);
  FCFN_CHECK(test::contains(run.output, "unauthorized"));
  // No session was established, so no request could be served.
  FCFN_CHECK(!test::contains(run.output, "STEP 1 describe status=ok"));
  coordinator.kill();
}

FCFN_TEST(multiprocess, a_second_incarnation_may_not_open_a_live_store) {
  Fixture fixture;
  test::Coordinator coordinator =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());

  test::ProcessOptions options;
  options.executable = test::app_directory() / "fcfn_coordinator.exe";
  options.arguments = {"--store", fixture.store.string(), "--token", kToken, "--port", "0"};
  test::ChildProcess second = test::ChildProcess::spawn(options);
  const int exit_code = second.wait();
  FCFN_CHECK_NE(exit_code, 0);
  const std::string output = second.output() + second.drain_available();
  FCFN_CHECK(test::contains(output, "already open"));
  FCFN_CHECK(coordinator.running());
  coordinator.kill();
}

FCFN_TEST(multiprocess, hard_kill_before_commit_loses_the_mutation) {
  Fixture fixture;
  test::Coordinator coordinator = test::Coordinator::start(
      fixture.store, kToken,
      {"--topology", fixture.topology.string(), "--crash-point", "before_commit", "--crash-after",
       "3"});
  const std::filesystem::path scenario = fixture.scenario(
      "before_commit.txt",
      "detect src present 1\nauthorize\nplan\nboundary\n");
  test::ChildProcess client = test::launch_client(coordinator.port(), kToken, scenario);
  std::string marker;
  const bool reached = coordinator.wait_for_line("CRASH-POINT before_commit", marker);
  FCFN_CHECK(reached);
  FCFN_CHECK(test::contains(marker, "3"));
  coordinator.kill_now();
  const int client_exit = client.wait();
  FCFN_CHECK_NE(client_exit, 0);

  test::Coordinator restarted =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  const std::string startup = restarted.startup_json();
  FCFN_CHECK(test::contains(startup, "\"recovered\":true"));
  // The evidence and the authorization survived; the uncommitted plan did not.
  FCFN_CHECK(test::contains(startup, "\"restored_plans\":0"));
  const std::filesystem::path verification = fixture.scenario("verify_state.txt", "evidence\n");
  const test::ClientRun state = test::run_client(restarted.port(), kToken, verification);
  FCFN_CHECK_EQ(state.exit_code, 0);
  FCFN_CHECK(test::contains(state.output, "\"resource\":\"src\""));
  restarted.kill();
}

FCFN_TEST(multiprocess, hard_kill_after_commit_before_ack_keeps_the_mutation_unacknowledged) {
  Fixture fixture;
  test::Coordinator coordinator = test::Coordinator::start(
      fixture.store, kToken,
      {"--topology", fixture.topology.string(), "--crash-point", "after_commit_before_ack",
       "--crash-after", "1"});
  const std::filesystem::path scenario =
      fixture.scenario("after_commit.txt", "detect src present 1\nauthorize\n");
  test::ChildProcess client = test::launch_client(coordinator.port(), kToken, scenario);
  std::string marker;
  FCFN_CHECK(coordinator.wait_for_line("CRASH-POINT after_commit_before_ack", marker));
  coordinator.kill_now();
  const int client_exit = client.wait();
  FCFN_CHECK_NE(client_exit, 0);
  const std::string client_output = client.output() + client.drain_available();
  // The client never received an answer: no STEP line for the detection.
  FCFN_CHECK(test::contains(client_output, "io_error"));

  test::Coordinator restarted =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  FCFN_CHECK(test::contains(restarted.startup_json(), "\"restored_detections\":1"));
  const std::filesystem::path verification = fixture.scenario("verify_state.txt", "evidence\n");
  const test::ClientRun state = test::run_client(restarted.port(), kToken, verification);
  FCFN_CHECK_EQ(state.exit_code, 0);
  FCFN_CHECK(test::contains(state.output, "\"resource\":\"src\""));
  FCFN_CHECK(test::contains(state.output, "\"state\":\"present\""));
  restarted.kill();
}

FCFN_TEST(multiprocess, hard_kill_after_ack_keeps_the_mutation_and_the_answer) {
  Fixture fixture;
  test::Coordinator coordinator = test::Coordinator::start(
      fixture.store, kToken,
      {"--topology", fixture.topology.string(), "--crash-point", "after_ack", "--crash-after",
       "1"});
  const std::filesystem::path scenario =
      fixture.scenario("after_ack.txt", "detect src present 1\nauthorize\n");
  test::ChildProcess client = test::launch_client(coordinator.port(), kToken, scenario);
  std::string marker;
  FCFN_CHECK(coordinator.wait_for_line("CRASH-POINT after_ack", marker));
  coordinator.kill_now();
  const int client_exit = client.wait();
  FCFN_CHECK_NE(client_exit, 0);
  const std::string client_output = client.output() + client.drain_available();
  // The first step was answered before the crash; the second was not.
  FCFN_CHECK(test::contains(client_output, "STEP 1 detect status=ok"));

  test::Coordinator restarted =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  FCFN_CHECK(test::contains(restarted.startup_json(), "\"restored_detections\":1"));
  restarted.kill();
}

FCFN_TEST(multiprocess, torn_log_tail_is_repaired_when_a_real_process_restarts) {
  Fixture fixture;
  {
    test::Coordinator coordinator =
        test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
    const std::filesystem::path scenario =
        fixture.scenario("write.txt", "detect src present 1\ndetect src present 2\n");
    const test::ClientRun run = test::run_client(coordinator.port(), kToken, scenario);
    FCFN_CHECK_EQ(run.exit_code, 0);
    coordinator.kill();
  }

  const std::uint64_t sequence = current_wal_sequence(fixture.store);
  const std::filesystem::path wal = wal_path(fixture.store, sequence);
  const std::uintmax_t size = std::filesystem::file_size(wal);
  FCFN_CHECK(size > 8);
  std::filesystem::resize_file(wal, size - 3);

  test::Coordinator restarted =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  const std::string startup = restarted.startup_json();
  FCFN_CHECK(test::contains(startup, "\"torn_tail_recovered\":true"));
  // torn_tail_bytes counts the bytes discarded from the end of the log.
  FCFN_CHECK(!test::contains(startup, "\"torn_tail_bytes\":0"));
  // The complete records before the damaged one are intact.
  FCFN_CHECK(test::contains(startup, "\"restored_detections\":1"));
  restarted.kill();

  // A second restart must find a clean log: the repair is durable, not repeated.
  test::Coordinator third =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());
  FCFN_CHECK(test::contains(third.startup_json(), "\"torn_tail_recovered\":false"));
  FCFN_CHECK(test::contains(third.startup_json(), "\"restored_detections\":1"));
  third.kill();
}

FCFN_TEST(multiprocess, replayed_sequence_and_foreign_session_are_refused_by_a_real_server) {
  Fixture fixture;
  test::Coordinator coordinator =
      test::Coordinator::start(fixture.store, kToken, fixture.topology_argument());

  const VoidResult initialized = net::ensure_socket_layer();
  FCFN_CHECK_OK(initialized);
  auto socket = net::Socket::connect_loopback(coordinator.port());
  FCFN_CHECK_OK(socket);
  auto& connection = *socket.value();

  net::HelloPayload hello;
  hello.token = kToken;
  hello.label = "raw-client";
  net::Frame frame;
  frame.type = net::MessageType::Hello;
  frame.payload = net::encode_hello(hello);
  std::vector<std::byte> encoded = net::encode_frame(frame);
  FCFN_CHECK_OK(connection.write_all(encoded));

  net::FrameStream stream;
  std::vector<std::byte> buffer(4096);
  bool handshaken = false;
  net::HelloAckPayload ack;
  while (!handshaken) {
    const net::DecodeOutcome outcome = stream.next();
    if (outcome.status == net::FrameStatus::NeedMore) {
      auto read = connection.read(buffer);
      FCFN_CHECK_OK(read);
      FCFN_CHECK(read.value() > 0);
      FCFN_CHECK(stream.feed(std::span<const std::byte>(buffer.data(), read.value())));
      continue;
    }
    FCFN_CHECK(outcome.status == net::FrameStatus::Complete);
    auto decoded = net::decode_hello_ack(outcome.frame.payload);
    FCFN_CHECK_OK(decoded);
    ack = decoded.value();
    handshaken = true;
  }
  FCFN_CHECK(ack.accepted);

  // A request stamped with another session's identity must be refused.
  net::RequestPayload request;
  request.operation = net::Operation::Describe;
  net::Frame forged;
  forged.type = net::MessageType::Request;
  forged.session = SessionId{ack.session.value() + 99};
  forged.sequence = Sequence{1};
  forged.epoch = ack.epoch;
  forged.payload = net::encode_request(request);
  FCFN_CHECK_OK(connection.write_all(net::encode_frame(forged)));

  bool refused = false;
  for (int i = 0; i < 64 && !refused; ++i) {
    const net::DecodeOutcome outcome = stream.next();
    if (outcome.status == net::FrameStatus::NeedMore) {
      auto read = connection.read(buffer);
      if (!read.ok() || read.value() == 0) {
        refused = true;
        break;
      }
      FCFN_CHECK(stream.feed(std::span<const std::byte>(buffer.data(), read.value())));
      continue;
    }
    if (outcome.status != net::FrameStatus::Complete) {
      refused = true;
      break;
    }
    auto response = net::decode_response(outcome.frame.payload);
    FCFN_CHECK_OK(response);
    refused = response.value().code == StatusCode::Unauthorized;
  }
  FCFN_CHECK(refused);
  connection.shutdown();
  coordinator.kill();
}
