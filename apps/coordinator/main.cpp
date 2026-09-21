// FCFN coordinator service.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config_text.hpp"
#include "fcfn/net/server.hpp"
#include "fcfn/runtime/coordinator.hpp"
#include "fcfn/version.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: fcfn_coordinator --token <token> [--store <dir>|--in-memory] [--port <n>]\n"
               "                        [--topology <file>] [--policy <file>]\n"
               "                        [--lease-millis <n>] [--no-current-boot-confirmation]\n"
               "                        [--crash-point <before_commit|after_commit_before_ack|after_ack>]\n"
               "                        [--crash-after <n>]\n");
}

struct Arguments {
  std::string token{};
  std::string store{};
  bool in_memory{false};
  std::uint16_t port{0};
  std::string topology{};
  std::string policy{};
  std::uint64_t lease_millis{30000};
  bool require_current_boot_confirmation{true};
  std::string crash_point{};
  std::uint64_t crash_after{0};
};

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto next = [&](std::string& target) {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      target = argv[++i];
    };
    if (flag == "--token") {
      next(arguments.token);
    } else if (flag == "--store") {
      next(arguments.store);
    } else if (flag == "--in-memory") {
      arguments.in_memory = true;
    } else if (flag == "--port") {
      std::uint64_t value = 0;
      if (i + 1 >= argc || !parse_u64(argv[++i], value) || value > 65535) {
        usage();
        return 2;
      }
      arguments.port = static_cast<std::uint16_t>(value);
    } else if (flag == "--topology") {
      next(arguments.topology);
    } else if (flag == "--policy") {
      next(arguments.policy);
    } else if (flag == "--lease-millis") {
      if (i + 1 >= argc || !parse_u64(argv[++i], arguments.lease_millis)) {
        usage();
        return 2;
      }
    } else if (flag == "--no-current-boot-confirmation") {
      arguments.require_current_boot_confirmation = false;
    } else if (flag == "--crash-point") {
      next(arguments.crash_point);
    } else if (flag == "--crash-after") {
      if (i + 1 >= argc || !parse_u64(argv[++i], arguments.crash_after)) {
        usage();
        return 2;
      }
    } else {
      usage();
      return 2;
    }
  }

  if (arguments.token.empty()) {
    usage();
    return 2;
  }
  if (!arguments.in_memory && arguments.store.empty()) {
    std::fprintf(stderr, "fcfn_coordinator: either --store or --in-memory is required\n");
    return 2;
  }

  fcfn::net::ServerConfig config;
  config.session_token = arguments.token;
  config.port = arguments.port;
  config.crash_point = arguments.crash_point;
  config.crash_after = arguments.crash_after;
  config.runtime.persist = !arguments.in_memory;
  config.runtime.store_root = arguments.store;
  config.runtime.authorization_lease_millis = arguments.lease_millis;
  config.runtime.require_evidence_confirmation_in_current_boot =
      arguments.require_current_boot_confirmation;

  if (!arguments.policy.empty()) {
    auto policy = fcfn::apps::parse_policy_file(arguments.policy);
    if (!policy.ok()) {
      std::fprintf(stderr, "fcfn_coordinator: %s\n", policy.status().to_string().c_str());
      return 1;
    }
    config.runtime.policy = policy.value();
  }
  if (!arguments.topology.empty()) {
    auto topology = fcfn::apps::parse_topology_file(arguments.topology);
    if (!topology.ok()) {
      std::fprintf(stderr, "fcfn_coordinator: %s\n", topology.status().to_string().c_str());
      return 1;
    }
    config.runtime.topology = topology.value();
  }

  auto server = fcfn::net::CoordinatorServer::start(config);
  if (!server.ok()) {
    std::fprintf(stderr, "fcfn_coordinator: %s\n", server.status().to_string().c_str());
    return 1;
  }

  std::printf("STARTUP %s\n", fcfn::runtime::to_json(server.value()->runtime().startup()).c_str());
  std::printf("LISTENING %u\n", static_cast<unsigned>(server.value()->port()));
  std::printf("READY\n");
  std::fflush(stdout);

  const fcfn::VoidResult served = server.value()->serve();
  if (!served.ok()) {
    std::fprintf(stderr, "fcfn_coordinator: %s\n", served.status().to_string().c_str());
    return 1;
  }
  std::printf("STOPPED\n");
  std::fflush(stdout);
  return 0;
}
