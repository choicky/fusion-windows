#pragma once
#include "fusion/core.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace fusion {
inline constexpr std::size_t frontend_operation_bound = 32;

// Link/lifecycle state of a backend that owns a transport to an out-of-process
// Host. D072 §13 keeps the ladder distinguishable:
//   Disconnected  no transport at all;
//   Connected     transport up, protocol handshake not complete;
//   Established   handshake complete, so a Host incarnation is known.
// "Established" is still NOT "semantic ready": a context binding is per
// incarnation and per connection and is tracked by its owner.
enum class LinkState { Disconnected, Connected, Established };

// Causal link identity. D072 §11 names these distinctions (HostInstanceId,
// ConnectionGeneration); the numeric representation is a PoC engineering
// constant, not a frozen production identifier. Both values are 0 when no
// incarnation is established. A backend without a transport reports
// {Established, 0, 0}: it has no incarnation to fence, so the lifecycle checks
// below are transparent for it and its single-bind behaviour is unchanged.
struct LinkStatus {
  LinkState state = LinkState::Disconnected;
  std::uint64_t host_incarnation = 0;
  std::uint64_t connection_generation = 0;
};

// POC-LOCAL / NOT ACCEPTED ARCHITECTURE: an in-process Host adapter interface,
// not a final wire schema, ownership policy, or D072 P0 resolution.
struct BackendStimulus {
  enum class Type { Character, Space, Backspace, Escape, Return };
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  Type type = Type::Character;
  char character = 0;
  // X11/Fcitx keysym and modifier bits, supplied by Windows translation.
  std::uint32_t keysym = 0;
  std::uint32_t modifiers = 0;
};
struct BackendOperation {
  enum class Type { SetPreedit, CandidateSnapshot, CommitText, ForwardKey };
  Type type = Type::SetPreedit;
  std::string text;
  std::vector<std::string> candidates;
  bool original_key = false; // ForwardKey refers exactly to the current physical event
};
struct BackendBatch {
  enum class Disposition { Consumed, Forward, Reject };
  std::uint64_t backend_session_id = 0;
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  Disposition disposition = Disposition::Reject;
  std::vector<BackendOperation> operations;
};
class Backend {
public:
  virtual ~Backend() = default;
  virtual bool bind(Binding binding) = 0;
  virtual bool reset(Binding binding) = 0;
  virtual void unbind(Binding binding) = 0;
  virtual bool healthy() const = 0;
  virtual std::uint64_t session_id() const = 0;
  virtual BackendBatch send(const BackendStimulus &stimulus) = 0;

  // Link lifecycle. Defaults describe a backend with no transport, so every
  // existing implementation (Mock, in-process Fcitx, test fakes) stays valid and
  // keeps exactly the Stage 4A lifecycle: established, no incarnation, one bind.
  virtual LinkStatus link_status() const { return LinkStatus{LinkState::Established, 0, 0}; }
  // Bounded, idempotent attempt to (re)establish transport + protocol handshake.
  // Returns true iff link_status().state == Established afterwards. Must do no
  // I/O when the link is already established, and must never block unboundedly.
  virtual bool ensure_link(std::chrono::milliseconds timeout) {
    (void)timeout;
    return true;
  }
};
#if defined(FUSION_FCITX_HOST_BUILD)
#define FUSION_FCITX_API __declspec(dllexport)
#elif defined(FUSION_WITH_FCITX5)
#define FUSION_FCITX_API __declspec(dllimport)
#else
#define FUSION_FCITX_API
#endif
FUSION_FCITX_API std::unique_ptr<Backend> make_fcitx_backend();
} // namespace fusion
