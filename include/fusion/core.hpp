#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fusion {

// Logical values for the portable model; not wire identifiers.
struct Binding {
  std::uint64_t context;
  std::uint64_t lifetime;
  std::uint64_t epoch;
  bool operator==(const Binding &) const = default;
};

struct Key {
  Binding binding;
  std::uint64_t sequence;
  bool operator==(const Key &) const = default;
};

enum class Kind { State, Effect };
enum class Terminal { Pending, Consumed, Applied, FailedNoEffect, Cancelled, Indeterminate };
enum class Result { Accepted, Stale, Invalid, Blocked, Duplicate };
enum class Pass { Clear, DrainRequired, RecoveryRequired };
enum class EditResult { AppliedReadback, RefusedNoMutation, Indeterminate };
enum class Readback { Applied, NoEffect };

struct Ticket {
  Key key;
  std::uint64_t serial;
  bool operator==(const Ticket &) const = default;
};

struct Record {
  Key key;
  Kind kind;
  Terminal terminal = Terminal::Pending;
  bool proposed = false;
  bool issued = false;
  bool uncertain = false;
  std::uint64_t required_state = 0;
  std::optional<Ticket> ticket;
};

// One Windows-side logical context; caller owns admission and document execution.
// Single-owner deterministic transitions: no I/O, clocks, threads or backend decisions.
class Core {
public:
  Core(Binding binding, std::size_t capacity);
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  Core(Core &&) = delete;
  Core &operator=(Core &&) = delete;
  std::optional<Key> own(Kind kind, bool policy_allows);
  Result consume_state(Key key, EditResult result);
  Result propose_effect(Key key, std::uint64_t required_state);
  std::optional<Ticket> begin_effect(Key key);
  Result complete_effect(Ticket ticket, EditResult result);
  Result reject(Key key);
  Result cancel(Key key);
  Result reconcile(Ticket ticket, Readback result);
  Result reconcile_state(Key key, Readback result);
  void disconnect();
  Result restart(std::uint64_t epoch);
  Result rebind();
  Result replace_context(Binding binding);
  void destroy_context();
  Pass probe_pass() const;
  // Retirement requires explicit caller acknowledgement; uncertainty is retained.
  Result acknowledge(Key key);
  const std::vector<Record> &records() const { return records_; }
  std::uint64_t consumed_state() const { return consumed_state_; }
  std::size_t capacity() const { return capacity_; }
  Binding binding() const { return binding_; }

private:
  Record *find(Key key);
  Record *find(Ticket ticket);
  bool applicable(Key key) const;
  bool prior_pending(Key key) const;
  void fence();
  Binding binding_;
  std::size_t capacity_;
  std::vector<Record> records_;
  std::uint64_t sequence_ = 0;
  std::uint64_t ticket_serial_ = 0;
  std::uint64_t consumed_state_ = 0;
  std::uint64_t effect_fence_ = 0;
  bool alive_ = true;
  bool ready_ = true;
};

} // namespace fusion
