// FCFN concurrency and lifecycle suite.
//
// Synchronisation is deterministic: std::barrier and condition variables with
// explicit predicates, never sleeps and never timeouts. Every test asserts a
// property that must hold for ANY interleaving, or a property that is checked
// after all participants have been joined.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fcfn/core/clock.hpp"
#include "fcfn/net/client.hpp"
#include "fcfn/net/server.hpp"
#include "fcfn/net/socket.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/store/store.hpp"
#include "temp_dir.hpp"
#include "test_harness.hpp"

namespace {

using namespace fcfn;  // NOLINT(google-build-using-namespace)

constexpr const char* kToken = "concurrency-token";

model::TopologySpec sample_topology(std::uint64_t generation) {
  model::TopologySpec spec;
  spec.generation = model::TopologyGeneration{generation};
  auto add = [&spec](const char* id, bool containable, bool obligation) {
    model::NodeSpec node;
    node.id = model::ResourceId::unchecked(id);
    node.containable = containable;
    node.protected_obligation = obligation;
    node.weight = 1;
    spec.nodes.push_back(std::move(node));
  };
  add("src", true, false);
  add("mid", true, false);
  add("obligation", false, true);
  auto link = [&spec](const char* from, const char* to) {
    model::EdgeSpec edge;
    edge.from = model::ResourceId::unchecked(from);
    edge.to = model::ResourceId::unchecked(to);
    edge.evidence = model::EdgeEvidence::Proven;
    edge.generation = model::EvidenceGeneration{1};
    spec.edges.push_back(std::move(edge));
  };
  link("src", "mid");
  link("mid", "obligation");
  return spec;
}

runtime::RuntimeConfig memory_config() {
  runtime::RuntimeConfig config;
  config.persist = false;
  config.topology = sample_topology(1);
  return config;
}

}  // namespace

FCFN_TEST(concurrency, concurrent_detections_produce_one_consistent_evidence_vector) {
  ManualClock clock;
  auto runtime = runtime::ContainmentRuntime::open(memory_config(), &clock);
  FCFN_CHECK_OK(runtime);
  runtime::ContainmentRuntime& service = *runtime.value();

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kPerThread = 16;
  std::barrier start(static_cast<std::ptrdiff_t>(kThreads));
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> rejected{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&service, &start, &accepted, &rejected, thread]() {
      start.arrive_and_wait();
      for (std::size_t index = 0; index < kPerThread; ++index) {
        model::FailureObservation observation;
        observation.resource = model::ResourceId::unchecked(
            "resource-" + std::to_string(thread) + "-" + std::to_string(index));
        observation.state = model::EvidenceState::Present;
        observation.generation = model::EvidenceGeneration{1};
        const auto recorded = service.record_detection(observation);
        if (recorded.ok()) {
          ++accepted;
        } else {
          ++rejected;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FCFN_CHECK_EQ(accepted.load(), kThreads * kPerThread);
  FCFN_CHECK_EQ(rejected.load(), static_cast<std::size_t>(0));
  const auto evidence = service.evidence();
  FCFN_CHECK_OK(evidence);
  FCFN_CHECK_EQ(evidence.value().entries().size(), kThreads * kPerThread);
  // The digest is over a canonically ordered vector, so a concurrent ingestion
  // order cannot change it.
  FCFN_CHECK(!evidence.value().digest().is_zero());
}

FCFN_TEST(concurrency, observed_authority_never_regresses_under_a_writer) {
  ManualClock clock;
  auto runtime = runtime::ContainmentRuntime::open(memory_config(), &clock);
  FCFN_CHECK_OK(runtime);
  runtime::ContainmentRuntime& service = *runtime.value();

  constexpr std::size_t kReaders = 4;
  constexpr std::size_t kWrites = 32;
  std::atomic<bool> writer_done{false};
  std::atomic<std::size_t> regressions{0};
  std::vector<std::thread> readers;
  for (std::size_t reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&service, &writer_done, &regressions]() {
      model::EvidenceRevision previous{};
      while (!writer_done.load()) {
        const model::AuthorityVector observed = service.authority();
        if (observed.evidence_revision < previous) {
          ++regressions;
        }
        previous = observed.evidence_revision;
      }
    });
  }
  for (std::size_t index = 0; index < kWrites; ++index) {
    model::FailureObservation observation;
    observation.resource = model::ResourceId::unchecked("writer-" + std::to_string(index));
    observation.state = model::EvidenceState::Present;
    observation.generation = model::EvidenceGeneration{1};
    FCFN_CHECK_OK(service.record_detection(observation));
  }
  writer_done.store(true);
  for (std::thread& thread : readers) {
    thread.join();
  }
  FCFN_CHECK_EQ(regressions.load(), static_cast<std::size_t>(0));
  FCFN_CHECK_EQ(service.authority().evidence_revision.value(), kWrites);
}

FCFN_TEST(concurrency, racing_apply_submissions_admit_exactly_one_attempt) {
  ManualClock clock;
  runtime::RuntimeConfig config = memory_config();
  auto runtime = runtime::ContainmentRuntime::open(config, &clock);
  FCFN_CHECK_OK(runtime);
  runtime::ContainmentRuntime& service = *runtime.value();

  // The runtime config already loaded this topology; re-applying the same
  // generation would (correctly) be refused as a stale definition.
  model::FailureObservation observation;
  observation.resource = model::ResourceId::unchecked("src");
  observation.state = model::EvidenceState::Present;
  observation.generation = model::EvidenceGeneration{1};
  FCFN_CHECK_OK(service.record_detection(observation));
  const auto authorization = service.authorize();
  FCFN_CHECK_OK(authorization);

  runtime::PlanRequest plan_request;
  plan_request.authorization = authorization.value().id;
  const auto plan = service.plan(plan_request);
  FCFN_CHECK_OK(plan);

  model::TransitionRequest transition;
  transition.kind = model::TransitionKind::Expand;
  transition.plan = plan.value().generation;
  transition.plan_digest = plan.value().digest();
  transition.expected_released = true;
  transition.authorization = authorization.value().id;
  const auto decision = service.transition(transition);
  FCFN_CHECK_OK(decision);
  FCFN_CHECK(decision.value().status == model::TransitionStatus::Accepted);

  constexpr std::size_t kRacers = 6;
  std::barrier start(static_cast<std::ptrdiff_t>(kRacers));
  std::atomic<std::size_t> admitted{0};
  std::atomic<std::size_t> refused{0};
  std::vector<std::thread> racers;
  for (std::size_t racer = 0; racer < kRacers; ++racer) {
    racers.emplace_back([&service, &start, &admitted, &refused, &plan, &authorization]() {
      runtime::SubmitApplyRequest request;
      request.authorization = authorization.value().id;
      request.plan = plan.value().generation;
      request.plan_digest = plan.value().digest();
      start.arrive_and_wait();
      const auto attempt = service.submit_apply(request);
      if (attempt.ok()) {
        ++admitted;
      } else {
        ++refused;
      }
    });
  }
  for (std::thread& thread : racers) {
    thread.join();
  }
  FCFN_CHECK_EQ(admitted.load(), static_cast<std::size_t>(1));
  FCFN_CHECK_EQ(refused.load(), kRacers - 1);
  FCFN_CHECK_EQ(service.attempts().size(), static_cast<std::size_t>(1));
}

FCFN_TEST(concurrency, two_stores_cannot_open_the_same_root) {
  test::TempDir directory{"concurrency-store"};
  const std::filesystem::path root = directory.child("store");

  auto first = store::DurableStore::open(store::DurableStore::Options{root, 4, true, true});
  FCFN_CHECK_OK(first);
  auto second = store::DurableStore::open(store::DurableStore::Options{root, 4, true, true});
  FCFN_CHECK_STATUS(second, StatusCode::AlreadyExists);

  first.value().reset();
  auto third = store::DurableStore::open(store::DurableStore::Options{root, 4, true, true});
  FCFN_CHECK_OK(third);
  third.value().reset();
}

FCFN_TEST(concurrency, socket_shutdown_is_idempotent_and_releases_blocked_reads) {
  const VoidResult initialized = net::ensure_socket_layer();
  FCFN_CHECK_OK(initialized);
  auto listener = net::Listener::bind_loopback(0);
  FCFN_CHECK_OK(listener);

  std::atomic<bool> accepted{false};
  std::unique_ptr<net::Socket> server_side;
  std::thread acceptor([&listener, &accepted, &server_side]() {
    auto connection = listener.value().accept();
    if (connection.ok()) {
      server_side = std::move(connection.value());
      accepted.store(true);
    }
  });

  auto client_side = net::Socket::connect_loopback(listener.value().port());
  FCFN_CHECK_OK(client_side);

  std::atomic<bool> read_returned{false};
  std::thread reader([&server_side, &read_returned]() {
    while (server_side == nullptr) {
      std::this_thread::yield();
    }
    std::vector<std::byte> buffer(64);
    const auto read = server_side->read(buffer);
    read_returned.store(true);
    FCFN_CHECK(!read.ok() || read.value() == 0);
  });

  // Closing the socket from another thread must release the blocked read.
  while (!accepted.load()) {
    std::this_thread::yield();
  }
  server_side->shutdown();
  reader.join();
  FCFN_CHECK(read_returned.load());
  // Shutdown is idempotent and safe to call twice.
  server_side->shutdown();
  FCFN_CHECK(!server_side->valid());
  client_side.value()->shutdown();
  listener.value().shutdown();
  acceptor.join();
}

FCFN_TEST(concurrency, concurrent_sessions_are_served_and_shutdown_joins_every_thread) {
  net::ServerConfig config;
  config.session_token = kToken;
  config.port = 0;
  config.runtime.persist = false;
  config.runtime.topology = sample_topology(1);
  auto server = net::CoordinatorServer::start(config);
  FCFN_CHECK_OK(server);
  const std::uint16_t port = server.value()->port();

  std::atomic<bool> serving{true};
  std::thread serving_thread([&server, &serving]() {
    const VoidResult served = server.value()->serve();
    serving.store(false);
    FCFN_CHECK(served.ok());
  });

  constexpr std::size_t kClients = 4;
  std::barrier start(static_cast<std::ptrdiff_t>(kClients));
  std::atomic<std::size_t> successful{0};
  std::vector<std::thread> clients;
  for (std::size_t client = 0; client < kClients; ++client) {
    clients.emplace_back([&start, &successful, port, client]() {
      net::ClientConfig client_config;
      client_config.port = port;
      client_config.token = kToken;
      client_config.label = "concurrency-" + std::to_string(client);
      auto session = net::CoordinatorClient::connect(client_config);
      if (!session.ok()) {
        return;
      }
      start.arrive_and_wait();
      net::RequestPayload request;
      request.operation = net::Operation::Describe;
      const auto response = session.value()->call(request.operation, {});
      if (response.ok() && !is_failure(response.value().code)) {
        ++successful;
      }
      session.value()->close();
    });
  }
  for (std::thread& thread : clients) {
    thread.join();
  }
  FCFN_CHECK_EQ(successful.load(), kClients);

  // Shutdown must release the accept loop and join every session thread.
  server.value()->request_shutdown();
  serving_thread.join();
  FCFN_CHECK(!serving.load());
  FCFN_CHECK_EQ(server.value()->session_count(), static_cast<std::size_t>(0));
}

FCFN_TEST(concurrency, plan_table_stays_bounded_under_repeated_planning) {
  ManualClock clock;
  runtime::RuntimeConfig config = memory_config();
  config.max_retained_plans = 4;
  auto runtime = runtime::ContainmentRuntime::open(config, &clock);
  FCFN_CHECK_OK(runtime);
  runtime::ContainmentRuntime& service = *runtime.value();
  model::FailureObservation observation;
  observation.resource = model::ResourceId::unchecked("src");
  observation.state = model::EvidenceState::Present;
  observation.generation = model::EvidenceGeneration{1};
  FCFN_CHECK_OK(service.record_detection(observation));

  constexpr std::size_t kRounds = 12;
  PlanGeneration last{};
  for (std::size_t round = 0; round < kRounds; ++round) {
    const auto authorization = service.authorize();
    FCFN_CHECK_OK(authorization);
    runtime::PlanRequest request;
    request.authorization = authorization.value().id;
    const auto plan = service.plan(request);
    FCFN_CHECK_OK(plan);
    FCFN_CHECK(plan.value().generation > last);
    last = plan.value().generation;
  }
  // Older plans are evicted, the newest is retained, and none is corrupt.
  const auto retained = service.plan_by_generation(last);
  FCFN_CHECK_OK(retained);
  const auto evicted = service.plan_by_generation(PlanGeneration{1});
  FCFN_CHECK_STATUS(evicted, StatusCode::NotFound);
  FCFN_CHECK_EQ(service.stats().plans_computed, kRounds);
}
