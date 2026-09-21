// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "config_text.hpp"

#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace fcfn::apps {
namespace {

constexpr std::size_t kMaxLines = 200000;
constexpr std::size_t kMaxLineLength = 512;

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

bool parse_bool(const std::string& text, bool& out) {
  if (text == "1" || text == "true" || text == "yes") {
    out = true;
    return true;
  }
  if (text == "0" || text == "false" || text == "no") {
    out = false;
    return true;
  }
  return false;
}

using Fields = std::unordered_map<std::string, std::string>;

Fields parse_fields(const std::vector<std::string>& parts, std::size_t first) {
  Fields fields;
  for (std::size_t i = first; i < parts.size(); ++i) {
    const std::size_t equals = parts[i].find('=');
    if (equals == std::string::npos) {
      continue;
    }
    fields.emplace(parts[i].substr(0, equals), parts[i].substr(equals + 1));
  }
  return fields;
}

Result<std::vector<std::string>> read_lines(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    return Status{StatusCode::NotFound, "cannot open configuration file"};
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (line.size() > kMaxLineLength) {
      return Status{StatusCode::LimitExceeded, "configuration line exceeds the bound"};
    }
    if (lines.size() >= kMaxLines) {
      return Status{StatusCode::LimitExceeded, "configuration file exceeds the line bound"};
    }
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace

Result<model::TopologySpec> parse_topology_file(const std::filesystem::path& path) {
  auto lines = read_lines(path);
  if (!lines.ok()) {
    return lines.status();
  }
  model::TopologySpec spec;
  for (const std::string& line : lines.value()) {
    const std::vector<std::string> parts = split(line);
    if (parts.empty()) {
      continue;
    }
    if (parts[0] == "generation" && parts.size() == 2) {
      std::uint64_t value = 0;
      if (!parse_u64(parts[1], value)) {
        return Status{StatusCode::InvalidArgument, "topology generation is malformed"};
      }
      spec.generation = model::TopologyGeneration{value};
    } else if (parts[0] == "node" && parts.size() >= 2) {
      auto id = model::ResourceId::parse(parts[1]);
      if (!id.ok()) {
        return id.status();
      }
      model::NodeSpec node;
      node.id = std::move(id.value());
      const Fields fields = parse_fields(parts, 2);
      for (const auto& entry : fields) {
        if (entry.first == "containable") {
          if (!parse_bool(entry.second, node.containable)) {
            return Status{StatusCode::InvalidArgument, "node containable flag is malformed"};
          }
        } else if (entry.first == "protected") {
          if (!parse_bool(entry.second, node.protected_obligation)) {
            return Status{StatusCode::InvalidArgument, "node protected flag is malformed"};
          }
        } else if (entry.first == "weight") {
          std::uint64_t value = 0;
          if (!parse_u64(entry.second, value)) {
            return Status{StatusCode::InvalidArgument, "node weight is malformed"};
          }
          node.weight = value;
        } else if (entry.first == "completeness") {
          model::AdjacencyCompleteness completeness{};
          if (!model::parse_adjacency_completeness(entry.second, completeness)) {
            return Status{StatusCode::InvalidArgument, "node completeness is malformed"};
          }
          node.completeness = completeness;
        } else {
          return Status{StatusCode::InvalidArgument, "unknown node field"};
        }
      }
      spec.nodes.push_back(std::move(node));
    } else if (parts[0] == "edge" && parts.size() >= 3) {
      auto from = model::ResourceId::parse(parts[1]);
      auto to = model::ResourceId::parse(parts[2]);
      if (!from.ok()) {
        return from.status();
      }
      if (!to.ok()) {
        return to.status();
      }
      model::EdgeSpec edge;
      edge.from = std::move(from.value());
      edge.to = std::move(to.value());
      edge.evidence = model::EdgeEvidence::Unknown;
      const Fields fields = parse_fields(parts, 3);
      for (const auto& entry : fields) {
        if (entry.first == "evidence") {
          model::EdgeEvidence evidence{};
          if (!model::parse_edge_evidence(entry.second, evidence)) {
            return Status{StatusCode::InvalidArgument, "edge evidence is malformed"};
          }
          edge.evidence = evidence;
        } else if (entry.first == "generation") {
          std::uint64_t value = 0;
          if (!parse_u64(entry.second, value)) {
            return Status{StatusCode::InvalidArgument, "edge generation is malformed"};
          }
          edge.generation = model::EvidenceGeneration{value};
        } else {
          return Status{StatusCode::InvalidArgument, "unknown edge field"};
        }
      }
      if (edge.generation.is_zero()) {
        edge.generation = model::EvidenceGeneration{1};
      }
      spec.edges.push_back(std::move(edge));
    } else {
      return Status{StatusCode::InvalidArgument, "unrecognised topology directive"};
    }
  }
  return spec;
}

Result<std::vector<model::FailureObservation>> parse_evidence_file(const std::filesystem::path& path) {
  auto lines = read_lines(path);
  if (!lines.ok()) {
    return lines.status();
  }
  std::vector<model::FailureObservation> observations;
  for (const std::string& line : lines.value()) {
    const std::vector<std::string> parts = split(line);
    if (parts.empty()) {
      continue;
    }
    if (parts.size() < 3) {
      return Status{StatusCode::InvalidArgument, "evidence line requires resource, state, generation"};
    }
    auto id = model::ResourceId::parse(parts[0]);
    if (!id.ok()) {
      return id.status();
    }
    model::EvidenceState state{};
    if (!model::parse_evidence_state(parts[1], state)) {
      return Status{StatusCode::InvalidArgument, "evidence state is malformed"};
    }
    std::uint64_t generation = 0;
    if (!parse_u64(parts[2], generation)) {
      return Status{StatusCode::InvalidArgument, "evidence generation is malformed"};
    }
    model::FailureObservation observation;
    observation.resource = std::move(id.value());
    observation.state = state;
    observation.generation = model::EvidenceGeneration{generation};
    if (parts.size() > 3) {
      const Fields fields = parse_fields(parts, 3);
      for (const auto& entry : fields) {
        if (entry.first == "digest") {
          Digest digest;
          if (!Digest::parse(entry.second, digest)) {
            return Status{StatusCode::InvalidArgument, "evidence digest is malformed"};
          }
          observation.source_digest = digest;
        } else if (entry.first == "at") {
          std::uint64_t at = 0;
          if (!parse_u64(entry.second, at)) {
            return Status{StatusCode::InvalidArgument, "evidence timestamp is malformed"};
          }
          observation.observed_at_millis = at;
        } else {
          return Status{StatusCode::InvalidArgument, "unknown evidence field"};
        }
      }
    }
    observations.push_back(std::move(observation));
  }
  return observations;
}

Result<model::ContainmentPolicy> parse_policy_file(const std::filesystem::path& path) {
  auto lines = read_lines(path);
  if (!lines.ok()) {
    return lines.status();
  }
  model::ContainmentPolicy policy = model::default_policy();
  for (const std::string& line : lines.value()) {
    const std::vector<std::string> parts = split(line);
    if (parts.empty()) {
      continue;
    }
    if (parts.size() != 2) {
      return Status{StatusCode::InvalidArgument, "policy directive requires a key and a value"};
    }
    std::uint64_t number = 0;
    const bool numeric = parse_u64(parts[1], number);
    if (parts[0] == "generation") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy generation must be a positive integer"};
      }
      policy.generation = model::PolicyGeneration{number};
    } else if (parts[0] == "require_failure_source_inclusion") {
      if (!parse_bool(parts[1], policy.require_failure_source_inclusion)) {
        return Status{StatusCode::InvalidArgument, "policy flag is malformed"};
      }
    } else if (parts[0] == "allow_protected_inclusion") {
      if (!parse_bool(parts[1], policy.allow_protected_inclusion)) {
        return Status{StatusCode::InvalidArgument, "policy flag is malformed"};
      }
    } else if (parts[0] == "minimize_protected_inclusions") {
      if (!parse_bool(parts[1], policy.minimize_protected_inclusions)) {
        return Status{StatusCode::InvalidArgument, "policy flag is malformed"};
      }
    } else if (parts[0] == "max_boundary_weight") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy weight bound is malformed"};
      }
      policy.max_boundary_weight = number;
    } else if (parts[0] == "max_boundary_members") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy member bound is malformed"};
      }
      policy.max_boundary_members = static_cast<std::size_t>(number);
    } else if (parts[0] == "exact_search_budget") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy search budget is malformed"};
      }
      policy.exact_search_budget = number;
    } else if (parts[0] == "heuristic_budget") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy heuristic budget is malformed"};
      }
      policy.heuristic_budget = number;
    } else if (parts[0] == "exact_instance_node_limit") {
      if (!numeric || number == 0) {
        return Status{StatusCode::InvalidArgument, "policy node limit is malformed"};
      }
      policy.exact_instance_node_limit = static_cast<std::size_t>(number);
    } else {
      return Status{StatusCode::InvalidArgument, "unknown policy field"};
    }
  }
  return policy;
}

VoidResult write_topology_file(const std::filesystem::path& path, const model::Topology& topology) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    return Status{StatusCode::IoError, "cannot open topology output file"};
  }
  stream << "generation " << topology.generation().value() << "\n";
  for (std::size_t i = 0; i < topology.node_count(); ++i) {
    const model::NodeSpec& node = topology.node(static_cast<model::NodeIndex>(i));
    stream << "node " << node.id.value() << " containable=" << (node.containable ? 1 : 0)
           << " weight=" << node.weight << " protected=" << (node.protected_obligation ? 1 : 0)
           << " completeness=" << model::to_string(node.completeness) << "\n";
  }
  for (std::size_t i = 0; i < topology.edge_count(); ++i) {
    const model::Topology::Edge& edge = topology.edge_by_index(static_cast<model::EdgeIndex>(i));
    stream << "edge " << topology.resource(edge.from).value() << " "
           << topology.resource(edge.to).value() << " evidence=" << model::to_string(edge.evidence)
           << " generation=" << edge.generation.value() << "\n";
  }
  if (!stream) {
    return Status{StatusCode::IoError, "topology output failed"};
  }
  return ok_result();
}

bool json_field_u64(const std::string& document, const std::string& key, std::uint64_t& out) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t position = document.find(needle);
  if (position == std::string::npos) {
    return false;
  }
  std::size_t cursor = position + needle.size();
  while (cursor < document.size() && document[cursor] == ' ') {
    ++cursor;
  }
  std::uint64_t value = 0;
  bool digits = false;
  while (cursor < document.size() && document[cursor] >= '0' && document[cursor] <= '9') {
    digits = true;
    value = value * 10ull + static_cast<std::uint64_t>(document[cursor] - '0');
    ++cursor;
  }
  if (!digits) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace fcfn::apps
