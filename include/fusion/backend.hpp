#pragma once
#include "fusion/core.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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

// POC-LOCAL / NOT ACCEPTED ARCHITECTURE: a Host adapter interface,
// not a final wire schema, ownership policy, or D072 P0 resolution.
//
// Stage 5 candidate actions are explicit non-key semantic stimuli: the caller
// asks the engine to select a page-local candidate or to turn the page. They are
// not keys and must never be simulated by injecting one (D072 §7.4, D082): what
// selection means (commit, partial selection, list rebuild) is decided only by
// the Fcitx candidate's own select(), and the resulting callbacks come back in
// the same ordered batch as a key dispatch's would.
struct BackendStimulus {
  enum class Type {
    Character,
    Space,
    Backspace,
    Escape,
    Return,
    SelectCandidate,
    PageNext,
    PagePrev
  };
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  Type type = Type::Character;
  char character = 0;
  // X11/Fcitx keysym and modifier bits, supplied by Windows translation.
  // Zero for a non-key stimulus: an action carries no key identity.
  std::uint32_t keysym = 0;
  std::uint32_t modifiers = 0;
  // Page-local candidate index for SelectCandidate; -1 for every other type.
  std::int32_t candidate_index = -1;
};

// Mirror of the Fcitx page contract (fcitx::CandidateLayoutHint, candidatelist.h)
// in declaration order. Windows projects it; it is not a second layout model.
enum class CandidateLayout : std::uint8_t { NotSet = 0, Vertical = 1, Horizontal = 2 };

// One list slot of the visible page. entries[i] always corresponds to
// CandidateList::candidate(i): a placeholder keeps its index (it is never
// displayed, but hit-testing must not shift the list index out of alignment with
// select()). label/text/comment are flattened UTF-8 display text; the segmented
// FormattedPreedit/Text style model is not carried by this projection yet.
struct CandidateEntry {
  std::string label;
  std::string text;
  std::string comment;
  bool placeholder = false;
};

// STATE for one candidate page. An empty page is the authoritative "no candidates"
// projection: Fcitx has no explicit hide signal, so the page snapshot of an empty
// InputPanel candidate slot is what closes the presentation.
struct CandidatePage {
  std::vector<CandidateEntry> entries;
  std::int32_t cursor_index = -1; // page-local highlight, -1 = nothing highlighted
  CandidateLayout layout = CandidateLayout::NotSet;
  bool has_prev = false; // canonical pager truth (PageableCandidateList)
  bool has_next = false;
  bool truncated = false; // transport bound hit, not a semantic decision
  bool empty() const { return entries.empty(); }
};

struct BackendOperation {
  enum class Type { SetPreedit, CandidateSnapshot, CommitText, ForwardKey };
  Type type = Type::SetPreedit;
  std::string text;
  // Set only for CandidateSnapshot; never a second copy of the candidate state.
  std::optional<CandidatePage> page;
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
