// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/store/store.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#include "fcfn/core/canonical.hpp"
#include "fcfn/core/hash.hpp"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace fcfn::store {
namespace {

constexpr const char* kCurrentMagic = "fcfn-store 1";

std::string hex16(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string{buffer};
}

std::filesystem::path snapshot_path_for(const std::filesystem::path& root, std::uint64_t sequence) {
  return root / ("snapshot-" + hex16(sequence) + ".fcfn");
}

std::filesystem::path wal_path_for(const std::filesystem::path& root, std::uint64_t sequence) {
  return root / ("wal-" + hex16(sequence) + ".fcfn");
}

struct CurrentPointer {
  std::uint64_t snapshot{0};
  std::uint64_t wal{0};
};

std::vector<std::byte> encode_current(const CurrentPointer& pointer) {
  std::string text = kCurrentMagic;
  text += "\nsnapshot ";
  text += std::to_string(pointer.snapshot);
  text += "\nwal ";
  text += std::to_string(pointer.wal);
  text += "\n";
  Crc32c crc;
  crc.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
  text += "crc ";
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08x", crc.value());
  text += buffer;
  text += "\n";
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

bool parse_line(std::string_view text, std::size_t& offset, std::string_view& line, std::string_view& key,
                std::string_view& value) {
  if (offset >= text.size()) {
    return false;
  }
  const std::size_t end = text.find('\n', offset);
  if (end == std::string_view::npos) {
    return false;
  }
  line = text.substr(offset, end - offset);
  offset = end + 1;
  const std::size_t space = line.find(' ');
  if (space == std::string_view::npos) {
    key = line;
    value = {};
    return true;
  }
  key = line.substr(0, space);
  value = line.substr(space + 1);
  return true;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10ull) {
      return false;
    }
    value = value * 10ull + digit;
  }
  out = value;
  return true;
}

Result<CurrentPointer> decode_current(std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > 4096) {
    return Status{StatusCode::Corrupt, "CURRENT pointer has an impossible size"};
  }
  std::string text(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    text[i] = static_cast<char>(bytes[i]);
  }

  const std::size_t crc_marker = text.rfind("crc ");
  if (crc_marker == std::string::npos) {
    return Status{StatusCode::Corrupt, "CURRENT pointer has no integrity field"};
  }
  const std::string_view crc_text(text.data() + crc_marker + 4, text.size() - crc_marker - 4);
  // The integrity field covers every byte before it.
  Crc32c crc;
  crc.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), crc_marker));
  char expected[16];
  std::snprintf(expected, sizeof(expected), "%08x", crc.value());
  std::string_view trimmed = crc_text;
  while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
    trimmed.remove_suffix(1);
  }
  if (trimmed != expected) {
    return Status{StatusCode::Corrupt, "CURRENT pointer integrity check failed"};
  }

  std::string_view view(text.data(), crc_marker);
  std::size_t offset = 0;
  std::string_view line;
  std::string_view key;
  std::string_view value;
  if (!parse_line(view, offset, line, key, value) || line != kCurrentMagic) {
    return Status{StatusCode::Corrupt, "CURRENT pointer header mismatch"};
  }
  CurrentPointer pointer;
  bool have_snapshot = false;
  bool have_wal = false;
  while (offset < view.size()) {
    if (!parse_line(view, offset, line, key, value)) {
      return Status{StatusCode::Corrupt, "CURRENT pointer is malformed"};
    }
    if (key == "snapshot") {
      if (!parse_u64(value, pointer.snapshot)) {
        return Status{StatusCode::Corrupt, "CURRENT snapshot sequence is malformed"};
      }
      have_snapshot = true;
    } else if (key == "wal") {
      if (!parse_u64(value, pointer.wal)) {
        return Status{StatusCode::Corrupt, "CURRENT log sequence is malformed"};
      }
      have_wal = true;
    } else {
      return Status{StatusCode::Corrupt, "CURRENT pointer has an unknown field"};
    }
  }
  if (!have_snapshot || !have_wal || pointer.wal == 0) {
    return Status{StatusCode::Corrupt, "CURRENT pointer is incomplete"};
  }
  return pointer;
}

Result<std::vector<std::byte>> encode_snapshot_file(std::span<const std::byte> payload) {
  CanonicalWriter writer;
  writer.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(kSnapshotFileMagic), 8));
  writer.u16(kSnapshotFormatVersion);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u32(Crc32c::compute(payload));
  writer.raw(payload);
  return writer.data();
}

Result<std::vector<std::byte>> decode_snapshot_file(std::span<const std::byte> bytes) {
  if (bytes.size() < 18) {
    return Status{StatusCode::Corrupt, "snapshot file is too short"};
  }
  for (std::size_t i = 0; i < 8; ++i) {
    if (bytes[i] != static_cast<std::byte>(static_cast<unsigned char>(kSnapshotFileMagic[i]))) {
      return Status{StatusCode::Corrupt, "snapshot file magic mismatch"};
    }
  }
  const auto version = static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[8]) |
                                                  (static_cast<std::uint16_t>(bytes[9]) << 8));
  if (version != kSnapshotFormatVersion) {
    return Status{StatusCode::VersionUnsupported, "snapshot format version is not supported"};
  }
  std::uint32_t length = 0;
  for (unsigned i = 0; i < 4; ++i) {
    length |= static_cast<std::uint32_t>(bytes[10 + i]) << (8u * i);
  }
  if (length > kMaxSnapshotPayloadBytes) {
    return Status{StatusCode::LimitExceeded, "snapshot payload length exceeds the bound"};
  }
  std::uint32_t crc = 0;
  for (unsigned i = 0; i < 4; ++i) {
    crc |= static_cast<std::uint32_t>(bytes[14 + i]) << (8u * i);
  }
  const std::size_t expected = 18u + static_cast<std::size_t>(length);
  if (bytes.size() < expected) {
    // A truncated snapshot cannot be repaired: what is missing is unknown.
    return Status{StatusCode::Corrupt, "snapshot file is shorter than its header declares"};
  }
  if (bytes.size() > expected) {
    return Status{StatusCode::TrailingGarbage, "snapshot file has bytes after its payload"};
  }
  const std::span<const std::byte> payload = bytes.subspan(18, length);
  if (Crc32c::compute(payload) != crc) {
    return Status{StatusCode::Corrupt, "snapshot payload integrity check failed"};
  }
  return std::vector<std::byte>(payload.begin(), payload.end());
}

}  // namespace

namespace {

std::uint64_t current_process_id() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

/// Conservative liveness check: a pid is considered live when the platform still
/// reports it. Pid reuse can only cause a spurious refusal, never a double open.
bool process_is_alive(std::uint64_t pid) {
  if (pid == current_process_id()) {
    return true;
  }
  if (pid == 0) {
    return false;
  }
#ifdef _WIN32
  HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) {
    return false;
  }
  DWORD code = 0;
  const bool alive = ::GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE;
  ::CloseHandle(handle);
  return alive;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

}  // namespace

DurableStore::~DurableStore() { release_lock(); }

VoidResult DurableStore::acquire_lock() {
  lock_path_ = options_.root / "LOCK";
  std::error_code error;
  if (std::filesystem::exists(lock_path_, error)) {
    const Result<std::vector<std::byte>> existing = read_file(lock_path_, 64);
    if (existing.ok()) {
      std::string text;
      for (const std::byte byte : existing.value()) {
        if (byte == std::byte{0}) {
          break;
        }
        text.push_back(static_cast<char>(byte));
      }
      std::uint64_t owner = 0;
      if (parse_u64(text, owner) && process_is_alive(owner)) {
        return Status{StatusCode::AlreadyExists, "store is already open by a live process"};
      }
    }
    const VoidResult removed = remove_file_if_present(lock_path_);
    if (!removed.ok()) {
      return removed.status();
    }
  }
  const std::string text = std::to_string(current_process_id());
  std::vector<std::byte> bytes(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  const VoidResult written = write_file_atomic(lock_path_, bytes, false);
  if (!written.ok()) {
    return written.status();
  }
  holds_lock_ = true;
  return ok_result();
}

void DurableStore::release_lock() noexcept {
  if (!holds_lock_) {
    return;
  }
  std::error_code error;
  std::filesystem::remove(lock_path_, error);
  holds_lock_ = false;
}

Result<std::unique_ptr<DurableStore>> DurableStore::open(const Options& options) {
  if (options.root.empty()) {
    return Status{StatusCode::InvalidArgument, "durable store requires a root directory"};
  }
  if (options.retained_snapshots == 0 || options.retained_snapshots > kMaxRetainedSnapshots) {
    return Status{StatusCode::InvalidArgument, "retained snapshot bound out of range"};
  }

  std::unique_ptr<DurableStore> store(new DurableStore());
  store->options_ = options;

  std::error_code error;
  const bool exists = std::filesystem::exists(options.root, error);
  if (error) {
    return Status{StatusCode::IoError, "cannot inspect the store root"};
  }
  if (!exists) {
    std::filesystem::create_directories(options.root, error);
    if (error) {
      return Status{StatusCode::IoError, "cannot create the store root"};
    }
  } else if (!std::filesystem::is_directory(options.root, error)) {
    return Status{StatusCode::InvalidArgument, "store root is not a directory"};
  }

  const VoidResult locked = store->acquire_lock();
  if (!locked.ok()) {
    return locked.status();
  }

  const std::filesystem::path current = options.root / "CURRENT";
  const bool current_exists = std::filesystem::exists(current, error);
  if (error) {
    return Status{StatusCode::IoError, "cannot inspect the CURRENT pointer"};
  }

  if (!current_exists) {
    // A missing pointer is only legitimate for a directory without store files.
    bool has_store_files = false;
    for (const auto& entry : std::filesystem::directory_iterator(options.root, error)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("snapshot-", 0) == 0 || name.rfind("wal-", 0) == 0) {
        has_store_files = true;
        break;
      }
    }
    if (error) {
      return Status{StatusCode::IoError, "cannot enumerate the store root"};
    }
    if (has_store_files) {
      return Status{StatusCode::Corrupt, "store files exist without a CURRENT pointer"};
    }
    const VoidResult initialized = store->initialize_fresh();
    if (!initialized.ok()) {
      return initialized.status();
    }
    return store;
  }

  const Result<std::vector<std::byte>> pointer_bytes = read_file(current, 4096);
  if (!pointer_bytes.ok()) {
    return pointer_bytes.status();
  }
  const Result<CurrentPointer> pointer = decode_current(pointer_bytes.value());
  if (!pointer.ok()) {
    return pointer.status();
  }
  store->snapshot_sequence_ = SnapshotSequence{pointer.value().snapshot};
  store->wal_segment_ = SnapshotSequence{pointer.value().wal};

  if (store->snapshot_sequence_.value() != 0) {
    const std::filesystem::path path = snapshot_path_for(options.root, store->snapshot_sequence_.value());
    if (!std::filesystem::exists(path, error)) {
      return Status{StatusCode::NotFound, "CURRENT points at a missing snapshot"};
    }
    const Result<std::vector<std::byte>> raw = read_file(path, kMaxSnapshotPayloadBytes + 64);
    if (!raw.ok()) {
      return raw.status();
    }
    const Result<std::vector<std::byte>> payload = decode_snapshot_file(raw.value());
    if (!payload.ok()) {
      return payload.status();
    }
    const DecodeOutcome snapshot_record = decode_record(payload.value());
    if (snapshot_record.status != FrameStatus::Ok ||
        snapshot_record.frame.type != RecordType::SnapshotPayload) {
      return Status{StatusCode::Corrupt, "snapshot payload is not a valid snapshot record"};
    }
    // The recovered payload is the record body, not the framed record.
    store->recovery_.snapshot_payload = snapshot_record.frame.payload;
    store->recovery_.snapshot_sequence = store->snapshot_sequence_;
    store->last_sequence_ = snapshot_record.frame.sequence;
  }

  const VoidResult replayed = store->replay_log();
  if (!replayed.ok()) {
    return replayed.status();
  }
  const VoidResult cleanup = store->cleanup_old_files();
  if (!cleanup.ok()) {
    return cleanup.status();
  }
  store->recovery_.last_sequence = store->last_sequence_;
  store->recovery_.wal_segment = store->wal_segment_;
  store->recovery_.detail = "recovered existing store";
  return store;
}

VoidResult DurableStore::initialize_fresh() {
  const std::filesystem::path wal = wal_path_for(options_.root, 1);
  const VoidResult created = write_file_atomic(wal, {}, options_.durable_commit);
  if (!created.ok()) {
    return created.status();
  }
  const std::vector<std::byte> pointer = encode_current(CurrentPointer{0, 1});
  const VoidResult published = write_file_atomic(options_.root / "CURRENT", pointer, options_.durable_commit);
  if (!published.ok()) {
    return published.status();
  }
  wal_segment_ = SnapshotSequence{1};
  snapshot_sequence_ = SnapshotSequence{0};
  last_sequence_ = Sequence{0};
  recovery_.fresh = true;
  recovery_.wal_segment = wal_segment_;
  recovery_.last_sequence = last_sequence_;
  recovery_.detail = "initialised empty store";
  return ok_result();
}

VoidResult DurableStore::replay_log() {
  const std::filesystem::path path = wal_path();
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    // A missing active segment is created on demand; it holds no records.
    return write_file_atomic(path, {}, options_.durable_commit);
  }
  const Result<std::vector<std::byte>> raw = read_file(path, kMaxWalFileBytes);
  if (!raw.ok()) {
    return raw.status();
  }

  std::span<const std::byte> remaining(raw.value().data(), raw.value().size());
  std::size_t consumed_total = 0;
  Sequence previous = Sequence{0};
  bool have_previous = false;
  for (;;) {
    const DecodeOutcome outcome = decode_record(remaining);
    if (outcome.status == FrameStatus::NoMoreData) {
      break;
    }
    if (outcome.status == FrameStatus::TornTail) {
      if (!options_.allow_torn_tail_recovery) {
        return Status{StatusCode::Corrupt, "log ends with a partial record and recovery is disabled"};
      }
      const std::vector<std::byte> prefix(raw.value().begin(),
                                          raw.value().begin() + static_cast<std::ptrdiff_t>(consumed_total));
      const VoidResult repaired = write_file_atomic(path, prefix, options_.durable_commit);
      if (!repaired.ok()) {
        return repaired.status();
      }
      recovery_.torn_tail = true;
      recovery_.torn_tail_bytes = static_cast<std::uint64_t>(remaining.size());
      recovery_.detail = "repaired a partial record at the end of the log";
      break;
    }
    if (outcome.status != FrameStatus::Ok) {
      return Status{outcome.status == FrameStatus::UnsupportedVersion ? StatusCode::VersionUnsupported
                                                                      : StatusCode::Corrupt,
                    outcome.detail};
    }
    if (have_previous && !(previous < outcome.frame.sequence)) {
      return Status{StatusCode::SequenceRegression, "log sequence did not advance"};
    }
    previous = outcome.frame.sequence;
    have_previous = true;
    consumed_total += outcome.consumed;
    remaining = remaining.subspan(outcome.consumed);

    if (!(outcome.frame.sequence > last_sequence_)) {
      // Already represented by the snapshot: replaying it would double-apply.
      ++recovery_.skipped_records;
      continue;
    }
    last_sequence_ = outcome.frame.sequence;
    replayed_.push_back(outcome.frame);
    ++recovery_.replayed_records;
  }
  recovery_.replayed_records = replayed_.size();
  return ok_result();
}

VoidResult DurableStore::cleanup_old_files() {
  std::error_code error;
  std::vector<std::uint64_t> snapshots;
  std::vector<std::uint64_t> logs;
  for (const auto& entry : std::filesystem::directory_iterator(options_.root, error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("snapshot-", 0) == 0 && name.size() > 9) {
      const std::string digits = name.substr(9, name.size() - 9 - 5);
      std::uint64_t value = 0;
      if (parse_u64(digits, value)) {
        snapshots.push_back(value);
      }
    } else if (name.rfind("wal-", 0) == 0 && name.size() > 4) {
      const std::string digits = name.substr(4, name.size() - 4 - 5);
      std::uint64_t value = 0;
      if (parse_u64(digits, value)) {
        logs.push_back(value);
      }
    }
  }
  if (error) {
    return Status{StatusCode::IoError, "cannot enumerate the store root"};
  }
  std::sort(snapshots.begin(), snapshots.end());
  std::sort(logs.begin(), logs.end());

  while (snapshots.size() > options_.retained_snapshots) {
    const std::uint64_t victim = snapshots.front();
    snapshots.erase(snapshots.begin());
    if (victim == snapshot_sequence_.value()) {
      continue;
    }
    const VoidResult removed = remove_file_if_present(snapshot_path_for(options_.root, victim));
    if (!removed.ok()) {
      return removed.status();
    }
  }
  for (const std::uint64_t value : logs) {
    if (value >= wal_segment_.value()) {
      continue;
    }
    const VoidResult removed = remove_file_if_present(wal_path_for(options_.root, value));
    if (!removed.ok()) {
      return removed.status();
    }
  }
  return ok_result();
}

VoidResult DurableStore::commit(RecordType type, std::span<const std::byte> payload) {
  if (payload.size() > kMaxRecordPayloadBytes) {
    return Status{StatusCode::LimitExceeded, "record payload exceeds the bound"};
  }
  const Sequence next{last_sequence_.value() + 1};
  if (next.value() <= last_sequence_.value()) {
    return Status{StatusCode::Exhausted, "record sequence exhausted"};
  }
  const std::vector<std::byte> encoded = encode_record(type, next, payload);
  const VoidResult appended = append_file_durable(wal_path(), encoded, options_.durable_commit);
  if (!appended.ok()) {
    return appended.status();
  }
  // Publish only after the append has been flushed.
  last_sequence_ = next;
  return ok_result();
}

VoidResult DurableStore::write_snapshot(std::span<const std::byte> payload) {
  if (payload.size() > kMaxSnapshotPayloadBytes) {
    return Status{StatusCode::LimitExceeded, "snapshot payload exceeds the bound"};
  }
  const std::vector<std::byte> record = encode_record(RecordType::SnapshotPayload, last_sequence_, payload);
  const Result<std::vector<std::byte>> file = encode_snapshot_file(record);
  if (!file.ok()) {
    return file.status();
  }

  const std::uint64_t next_snapshot = snapshot_sequence_.value() + 1;
  const std::uint64_t next_wal = wal_segment_.value() + 1;
  const VoidResult written =
      write_file_atomic(snapshot_path_for(options_.root, next_snapshot), file.value(), options_.durable_commit);
  if (!written.ok()) {
    return written.status();
  }
  const VoidResult created = write_file_atomic(wal_path_for(options_.root, next_wal), {}, options_.durable_commit);
  if (!created.ok()) {
    return created.status();
  }
  const std::vector<std::byte> pointer = encode_current(CurrentPointer{next_snapshot, next_wal});
  const VoidResult published = write_file_atomic(options_.root / "CURRENT", pointer, options_.durable_commit);
  if (!published.ok()) {
    return published.status();
  }
  snapshot_sequence_ = SnapshotSequence{next_snapshot};
  wal_segment_ = SnapshotSequence{next_wal};
  recovery_.snapshot_sequence = snapshot_sequence_;
  recovery_.wal_segment = wal_segment_;
  return cleanup_old_files();
}

std::filesystem::path DurableStore::wal_path() const { return wal_path_for(options_.root, wal_segment_.value()); }

std::filesystem::path DurableStore::snapshot_path() const {
  return snapshot_path_for(options_.root, snapshot_sequence_.value());
}

std::filesystem::path DurableStore::current_path() const { return options_.root / "CURRENT"; }

}  // namespace fcfn::store
