// FCFN - durable store: write-ahead log, snapshots, and conservative recovery.
//
// Ordering contract for every durable mutation:
//   append to the write-ahead log -> flush to stable storage -> publish to
//   memory -> (optionally) snapshot.
// A mutation is durable only after its flush returns. Recovery replays only
// records whose sequence is beyond the snapshot's last applied sequence, refuses
// corrupt or impossible records instead of truncating them, and repairs exactly
// one damage class: a strict prefix of a record at the end of the log.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef FCFN_STORE_STORE_HPP
#define FCFN_STORE_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fcfn/core/ids.hpp"
#include "fcfn/core/result.hpp"
#include "fcfn/store/record.hpp"

namespace fcfn::store {

/// Upper bound applied to a whole write-ahead log segment before reading it.
inline constexpr std::uint64_t kMaxWalFileBytes = 512ull * 1024ull * 1024ull;

class DurableStore {
 public:
  struct Options {
    std::filesystem::path root{};
    /// Snapshot files retained after a rotation.
    std::size_t retained_snapshots{kMaxRetainedSnapshots};
    /// Flush to stable storage on every commit. Tests disable this only to
    /// demonstrate the difference between buffered and durable writes.
    bool durable_commit{true};
    /// Recover a strict prefix of a record at the end of the log. Never applies
    /// to integrity failures inside a complete record.
    bool allow_torn_tail_recovery{true};
  };

  struct RecoveryReport {
    bool fresh{false};
    bool torn_tail{false};
    std::uint64_t torn_tail_bytes{0};
    std::size_t replayed_records{0};
    std::size_t skipped_records{0};
    SnapshotSequence snapshot_sequence{};
    SnapshotSequence wal_segment{};
    Sequence last_sequence{};
    std::vector<std::byte> snapshot_payload{};
    std::string detail{};
  };

  /// Open a store. Acquires an exclusive incarnation lock on the store root: a
  /// second runtime may not open the same store while the first is live, because
  /// two writers would interleave a single log.
  [[nodiscard]] static Result<std::unique_ptr<DurableStore>> open(const Options& options);

  ~DurableStore();

  [[nodiscard]] const Options& options() const noexcept { return options_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const std::vector<Frame>& replayed_frames() const noexcept { return replayed_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return options_.root; }

  /// Append, flush, and publish one record. The next sequence is assigned here.
  [[nodiscard]] VoidResult commit(RecordType type, std::span<const std::byte> payload);

  /// Write a snapshot of the durable state and rotate the log segment.
  [[nodiscard]] VoidResult write_snapshot(std::span<const std::byte> payload);

  [[nodiscard]] Sequence last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] SnapshotSequence wal_segment() const noexcept { return wal_segment_; }
  [[nodiscard]] SnapshotSequence snapshot_sequence() const noexcept { return snapshot_sequence_; }

  /// Path of the currently active log segment.
  [[nodiscard]] std::filesystem::path wal_path() const;
  [[nodiscard]] std::filesystem::path snapshot_path() const;
  [[nodiscard]] std::filesystem::path current_path() const;

 private:
  DurableStore() = default;

  [[nodiscard]] VoidResult acquire_lock();
  void release_lock() noexcept;
  [[nodiscard]] VoidResult initialize_fresh();
  [[nodiscard]] VoidResult replay_log();
  [[nodiscard]] VoidResult cleanup_old_files();

  Options options_{};
  RecoveryReport recovery_{};
  std::vector<Frame> replayed_{};
  Sequence last_sequence_{};
  SnapshotSequence wal_segment_{};
  SnapshotSequence snapshot_sequence_{};
  std::filesystem::path lock_path_{};
  bool holds_lock_{false};
};

}  // namespace fcfn::store

#endif  // FCFN_STORE_STORE_HPP
