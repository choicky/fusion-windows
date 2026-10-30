#pragma once
#include "fusion/core.hpp"
#include "fusion/settings.hpp"
#include <algorithm>
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

// Where an establishment attempt stopped, and why. Diagnostics only: these are
// reported when a link cannot be established, so that "the Host is unreachable"
// becomes the stage that failed, the Win32 code of the call that failed, and —
// for the identity check — whether an API failed or the server simply was a
// different user or session. Nothing here is semantic state and nothing reads it
// to decide behaviour.
enum class ConnectStage {
  None,
  PipeName,     // the per-user pipe name could not be computed
  Open,         // CreateFileW on the pipe failed
  PipeMode,     // SetNamedPipeHandleState failed
  Identity,     // verify_server_identity refused the server
  HelloWrite,   // the Hello could not be written
  HelloRead,    // the Hello was written and no reply arrived
  HelloProtocol, // a reply arrived and was refused (payload or version)
};

enum class IdentityFailure {
  None,
  ServerProcessId, // GetNamedPipeServerProcessId failed
  ServerSession,   // ProcessIdToSessionId(server) failed
  MySession,       // ProcessIdToSessionId(self) failed
  SessionMismatch, // the server is in another session
  OpenProcess,     // OpenProcess(server) failed: an access check, not a mismatch
  OpenToken,       // OpenProcessToken(server) failed
  TokenUser,       // GetTokenInformation(TokenUser) failed on either side
  UserMismatch,    // both SIDs were read, and they differ
  // The AppContainer path (see pipe_security.hpp): it reads the pipe object instead of
  // the server process, so its failures name the object's properties.
  TokenIsAppContainer, // GetTokenInformation(TokenIsAppContainer) failed
  TokenSession,        // GetTokenInformation(TokenSessionId) failed
  PipeServerSession,   // GetNamedPipeServerSessionId failed
  PipeOwner,           // GetSecurityInfo(OWNER|LABEL) failed
  PipeOwnerMismatch,   // the pipe's owner SID is not this process's user
  PipeLabel,           // the mandatory label could not be read
  PipeIntegrityLow,    // the pipe carries a label below Medium
};

// The Win32 code a deadline expiry reports in ConnectFailure::win32_error.
//
// A read or write that the transport never completed is reported as ERROR_TIMEOUT
// (1460) and nothing else. It is not WAIT_TIMEOUT: that is the wait's own return
// value, not an error code, and it is not the ERROR_OPERATION_ABORTED (995) that the
// cancellation and handle cleanup on the way out would otherwise leave in the
// thread's last-error slot — which is exactly the wrong answer the diagnostic used to
// report. The value is spelled out so this header stays free of <windows.h>.
inline constexpr unsigned long transport_timeout_error = 1460; // ERROR_TIMEOUT

struct ConnectFailure {
  ConnectStage stage = ConnectStage::None;
  IdentityFailure identity = IdentityFailure::None;
  unsigned long win32_error = 0; // captured at the failing call; 0 when none
};

inline const char *connect_stage_name(ConnectStage stage) {
  switch (stage) {
  case ConnectStage::None:
    return "none";
  case ConnectStage::PipeName:
    return "pipe_name";
  case ConnectStage::Open:
    return "open";
  case ConnectStage::PipeMode:
    return "pipe_mode";
  case ConnectStage::Identity:
    return "identity";
  case ConnectStage::HelloWrite:
    return "hello_write";
  case ConnectStage::HelloRead:
    return "hello_read";
  case ConnectStage::HelloProtocol:
    return "hello_protocol";
  }
  return "?";
}

inline const char *identity_failure_name(IdentityFailure why) {
  switch (why) {
  case IdentityFailure::None:
    return "none";
  case IdentityFailure::ServerProcessId:
    return "server_process_id";
  case IdentityFailure::ServerSession:
    return "server_session";
  case IdentityFailure::MySession:
    return "my_session";
  case IdentityFailure::SessionMismatch:
    return "session_mismatch";
  case IdentityFailure::OpenProcess:
    return "open_process";
  case IdentityFailure::OpenToken:
    return "open_token";
  case IdentityFailure::TokenUser:
    return "token_user";
  case IdentityFailure::UserMismatch:
    return "user_mismatch";
  case IdentityFailure::TokenIsAppContainer:
    return "token_is_appcontainer";
  case IdentityFailure::TokenSession:
    return "token_session";
  case IdentityFailure::PipeServerSession:
    return "pipe_server_session";
  case IdentityFailure::PipeOwner:
    return "pipe_owner";
  case IdentityFailure::PipeOwnerMismatch:
    return "pipe_owner_mismatch";
  case IdentityFailure::PipeLabel:
    return "pipe_label";
  case IdentityFailure::PipeIntegrityLow:
    return "pipe_integrity_low";
  }
  return "?";
}

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
    // SelectCandidate with `candidate_index` read as the engine's **global** index across
    // the whole candidate list instead of the current page. Only a window that carries
    // more than one engine page needs it: in such a window a clicked cell may belong to a
    // page the engine is not on, and its page-local number does not exist. The selection
    // itself is the same call on the same engine word — `CandidateWord::select`, which
    // commits whatever the engine meant to commit — resolved through
    // `BulkCandidateList::candidateFromAll`. Nothing is paged or re-ordered to reach it.
    SelectCandidateGlobal,
    PageNext,
    PagePrev,
    // The key-up half of a modifier-only key. It carries the same keysym identity as the
    // down that preceded it and asks Fcitx one question it alone can answer: did this
    // press complete a configured modifier-only binding (`Hotkey/AltTriggerKeys`)?
    KeyRelease,
    // Move the engine's own candidate cursor one step, left or right. The engine owns the
    // cursor (CommonCandidateList is a CursorMovableCandidateList), so this asks it to move
    // rather than keeping a second highlight anywhere; a window that shows several engine
    // rows needs it because Left/Right are otherwise the engine's pinyin-string keys.
    CursorPrev,
    CursorNext
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
  // How many complete candidate rows the presentation can show right now, sent with every
  // key because the answer changes with the work area, the display's DPI and the user's font
  // size — none of which the Host can measure. 0 means "no opinion": the Host then uses the
  // maximum (five). Only the automatic scrolling arrangement reads it; every other
  // arrangement ignores it, and the engine never sees it.
  //
  // It is a *capacity*, not a request: the Host groups rows into scroll pages of this size,
  // so the row the paging keys are on and the rows on screen stay the same thing. A short
  // screen that shows three rows therefore turns pages of three, instead of turning pages of
  // five and hiding two of them.
  std::int32_t window_rows = 0;
};

// Mirror of the Fcitx page contract (fcitx::CandidateLayoutHint, candidatelist.h)
// in declaration order. Windows projects it; it is not a second layout model.
enum class CandidateLayout : std::uint8_t { NotSet = 0, Vertical = 1, Horizontal = 2 };

// How the Windows window arranges what the page carries. This is *presentation*, and it is
// deliberately not a fourth CandidateLayout: that enum mirrors the engine's own hint
// vocabulary (fcitx::CandidateLayoutHint), and the engine has no "scroll" hint to mirror.
//
// Page   — what the engine gave, arranged the way CandidateLayout says: one row
//          (Horizontal) or one column (Vertical). Every page sent before this existed is
//          this, and it is also the fallback when the list cannot be read beyond its page.
// Scroll — the page carries a bounded window of several engine pages (see the page fields
//          below), arranged as a grid whose columns are the engine's own PageSize. Each
//          row of the grid is one engine page, so the row the paging keys act on and the
//          numbers the engine would accept stay the engine's.
enum class CandidateArrangement : std::uint8_t { Page = 0, Scroll = 1 };

// One list slot of the visible page. entries[i] always corresponds to
// CandidateList::candidate(i): a placeholder keeps its index (it is never
// displayed, but hit-testing must not shift the list index out of alignment with
// select()). label/text/comment are flattened UTF-8 display text (each already
// passed through Instance::outputFilter by the projection).
struct CandidateEntry {
  std::string label;
  std::string text;
  std::string comment;
  bool placeholder = false;
};

// The font the Windows candidate popup draws with. It is presentation, not input
// semantics: the value comes from the candidate-presentation configuration the Host
// owns, and travels with the page instead of being cached per connection so a changed
// setting takes effect on the next projected page rather than at the next reconnect.
// Zero/empty means "this page carries no preference" — the case for Mock and test pages —
// and the popup then uses its own default, which is the same value the schema defaults to.
struct CandidateFont {
  std::string family;
  int size_pt = 0; // logical points, not device pixels: the popup scales it by monitor DPI
};

// STATE for one InputPanel update as the popup presentation shows it: the visible
// candidate page plus the panel text rows a mature Fcitx panel renders around it
// (classic-ui: upper row = auxUp + panel preedit, lower row = auxDown). The
// client preedit is not part of it: that is the in-document composition and is
// carried by SetPreedit. Fcitx has no explicit hide signal; the snapshot of an
// InputPanel whose displayed parts are all empty is what closes the presentation.
struct CandidatePage {
  std::vector<CandidateEntry> entries;
  std::int32_t cursor_index = -1; // page-local highlight, -1 = nothing highlighted
  CandidateLayout layout = CandidateLayout::NotSet;
  CandidateArrangement arrangement = CandidateArrangement::Page;
  CandidateFont font; // presentation only; never a candidate identity or a selection
  // The engine's own paging facts, carried so the window can be arranged without a second
  // notion of "page". All zero when the page does not come from a paging-aware list (Mock,
  // a list without PageableCandidateList, or a window that could not be read):
  //
  //   page_size          fcitx::CommonCandidateList::pageSize() — the columns of one row
  //   current_page       the engine's page right now — the row the paging keys act on
  //   window_first_page  the engine page entries[0] belongs to
  //   total_pages        how many pages the list has
  //
  // Scroll pages are the only ones that carry a non-zero window, and entries[] is then
  // (window_last_page - window_first_page + 1) * page_size long, in engine order. For a
  // Page arrangement entries[] is exactly the engine page and window_first_page is 0.
  std::int32_t page_size = 0;
  std::int32_t current_page = 0;
  std::int32_t window_first_page = 0;
  std::int32_t total_pages = 0;
  bool has_prev = false; // canonical pager truth (PageableCandidateList)
  bool has_next = false;
  bool truncated = false; // transport bound hit, not a semantic decision
  // Panel text rows, flattened UTF-8 after Instance::outputFilter. Empty when the
  // engine left the field empty.
  std::string aux_up;
  std::string panel_preedit; // InputPanel::preedit(), the server-side preedit row
  std::string aux_down;
  // No candidate slots: candidate actions have nothing to act on.
  bool empty() const { return entries.empty(); }
  // Nothing the panel presentation would show (classic-ui visibility rule: a
  // non-placeholder candidate or non-empty upper/lower row). This, not
  // empty(), is the hide signal.
  bool panel_empty() const {
    for (const auto &entry : entries)
      if (!entry.placeholder)
        return false;
    return aux_up.empty() && panel_preedit.empty() && aux_down.empty();
  }
  // Transport/presentation normalization for a page that a bound truncated to a
  // retained prefix: an engine cursor referring to an entry that was not
  // retained becomes no-highlight (-1); a cursor inside the retained prefix is
  // preserved. It never highlights a different candidate in its place and never
  // touches the Fcitx engine cursor — the next engine re-read projects the
  // engine's own truth. Without this, a truncated page would carry a cursor the
  // wire validator must reject, losing the whole presentation.
  void normalize_cursor_to_retained_prefix() {
    if (cursor_index >= static_cast<std::int32_t>(entries.size()))
      cursor_index = -1;
  }
  // The engine's global index of a displayed entry, and the row the engine's current page
  // occupies inside the window. Both are pure arithmetic over the paging facts above, so
  // the window never invents an ordering: entries[] is already the engine's.
  bool paging_known() const { return page_size > 0; }
  std::int32_t global_index_of(std::size_t entry_index) const {
    if (!paging_known() || entry_index > static_cast<std::size_t>(INT32_MAX))
      return -1;
    return window_first_page * page_size + static_cast<std::int32_t>(entry_index);
  }
  std::int32_t current_row() const {
    if (!paging_known())
      return 0;
    return (std::max)(0, current_page - window_first_page);
  }
};

// Mirror of fcitx::TextFormatFlag bit values (textformatflags.h). Windows carries
// the engine's bits; it does not define its own style vocabulary.
namespace text_format {
inline constexpr std::uint32_t underline = 1u << 3;
inline constexpr std::uint32_t highlight = 1u << 4;
inline constexpr std::uint32_t dont_commit = 1u << 5;
inline constexpr std::uint32_t bold = 1u << 6;
inline constexpr std::uint32_t strike = 1u << 7;
inline constexpr std::uint32_t italic = 1u << 8;
inline constexpr std::uint32_t known = underline | highlight | dont_commit | bold | strike | italic;
} // namespace text_format

// One fcitx::Text piece of the displayed preedit: its UTF-8 byte length inside
// BackendOperation::text and its TextFormatFlag bits. Segments tile the text in
// order, so they never duplicate the string itself.
struct TextSegment {
  std::uint32_t bytes = 0;
  std::uint32_t format = 0;
};

// The fcitx::Text contract of the displayed (client) preedit after
// Instance::outputFilter: the segment/format list and the engine cursor as a
// UTF-8 byte offset into the flattened text, -1 = hidden (text.h).
struct FormattedPreedit {
  std::vector<TextSegment> segments;
  std::int32_t cursor = -1;
  // Segments tile exactly `text`, and the cursor is hidden or inside it.
  bool consistent_with(const std::string &text) const {
    std::size_t total = 0;
    for (const auto &segment : segments)
      total += segment.bytes;
    return total == text.size() && cursor >= -1 &&
           (cursor < 0 || static_cast<std::size_t>(cursor) <= text.size());
  }
};

struct BackendOperation {
  enum class Type { SetPreedit, CandidateSnapshot, CommitText, ForwardKey };
  Type type = Type::SetPreedit;
  std::string text; // SetPreedit: the flattened displayed preedit
  // Set only for CandidateSnapshot; never a second copy of the candidate state.
  std::optional<CandidatePage> page;
  bool original_key = false; // ForwardKey refers exactly to the current physical event
  // Set only for SetPreedit from a frontend that observes the fcitx::Text contract.
  // Absent means plain text with the caret at its end (Mock Backend, test doubles).
  std::optional<FormattedPreedit> preedit;
};
struct BackendBatch {
  enum class Disposition { Consumed, Forward, Reject };
  std::uint64_t backend_session_id = 0;
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  Disposition disposition = Disposition::Reject;
  // For a Forward: whether this context had already been active when the engine answered.
  // A key the engine declines in an active context is the engine's own "not mine"; the same
  // answer before the context was ever active belongs to the key that activates it, and that
  // key stays the application's. The frontend needs the difference because a key it claimed
  // in T cannot be handed back — it may only deliver what an active engine declined.
  bool context_was_active = false;
  std::vector<BackendOperation> operations;
};
// R52: one observed move of a context's engine state — which input method the engine resolves
// for it, and whether that answer is an active one rather than the group's off slot. The Host
// reports it, because an input-method name never reaches the TIP: what crosses the pipe is the
// single `context_was_active` flag above, and the name stays in the engine. `trigger` names the
// Host's own sample point (a context entering use, a key, a modifier release, a page action),
// not a claim about what caused the move.
struct EngineStateChange {
  std::uint64_t client_context = 0; // the id the owning client bound this context with, so the
  std::uint32_t owner_pid = 0;      // TIP log and the Host log join on it; 0 when no pipe said
  bool active = false;
  std::string input_method;
  const char *trigger = "unknown";
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

  // Diagnostics only: where the last establishment attempt stopped. A backend
  // without a transport has nothing to report and keeps the default.
  virtual ConnectFailure last_connect_failure() const { return {}; }

  // Diagnostics only: the engine-state changes observed since the last call, oldest first. The
  // Host server drains it after the request that produced them and writes one diagnostic line per
  // change: that is where the Host's own log is, while the pair itself only exists inside the
  // engine. A backend that runs no engine keeps the default, which costs one empty vector.
  virtual std::vector<EngineStateChange> take_engine_state_changes() { return {}; }

  // R51: which process the Host's pipe saw when this context was asked for, 0 when it has no
  // pipe or the pipe would not say. A refusal that later waits for that context names it, so the
  // user can see which window to return to. R52 adds the id the *client* used for the same
  // context: the two logs are joined on it, because the Host's own id is the one the client never
  // sees. Naming only: no bind decision, no key and no composition reads it back, and a backend
  // that does not hold contexts keeps the default.
  virtual void note_context_owner(std::uint64_t context, std::uint64_t client_context,
                                  std::uint32_t client_pid) {
    (void)context;
    (void)client_context;
    (void)client_pid;
  }

  // Settings control calls are separate from the input stimulus path. Production
  // FcitxBackend implementations execute them on the Fcitx event loop and keep
  // settings admission separate from key admission so opening the UI never
  // blocks or resets a text context. Non-Fcitx test backends default to an
  // explicit unsupported result.
  virtual SettingsStatus settings_get_config(const std::string &, SettingsSnapshot &,
                                             std::string &error) {
    error = "settings are not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_set_config(const std::string &, const SettingsValue &,
                                             SettingsSnapshot &, std::string &error) {
    error = "settings are not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_get_selection(SettingsSelection &, std::string &error) {
    error = "input-method selection is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_select_input_method(const std::string &, SettingsSelection &,
                                                       std::string &error) {
    error = "input-method selection is not supported by this backend";
    return SettingsStatus::Unsupported;
  }

  // Stage A input-method list management. Get reads the current group's ordered
  // enabled list plus the full available set; Set writes an ordered enabled list
  // back through the manager's official setGroup/save. These are separate from the
  // single-select path above: selection changes which method a context follows,
  // list management changes which methods belong to the group and their order.
  virtual SettingsStatus settings_get_im_list(ImList &, std::string &error) {
    error = "input-method list management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_set_im_list(const std::vector<std::string> &, ImList &,
                                              std::string &error) {
    error = "input-method list management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }

  // Pinyin dictionary management (settings_api_version 3). The list reads the two data
  // roots the engine itself searches for `pinyin/dictionaries/*.dict`; import converts a
  // text dictionary through libime and places the result in the per-user root; enable and
  // disable write the engine's own `<stem>.disable` marker; delete removes a user
  // dictionary and its marker; reload re-runs the addon's `loadExtraDict()` through its
  // `dictmanager` sub-config. Every mutating call returns the list it left behind, so the
  // window never keeps a second copy of what is on disk.
  virtual SettingsStatus settings_list_dictionaries(DictionaryList &, std::string &error) {
    error = "dictionary management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_import_dictionary(const std::string &, bool, DictionaryEntry &,
                                                    DictionaryList &, std::string &error) {
    error = "dictionary management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_set_dictionary_enabled(const std::string &, bool,
                                                         DictionaryList &, std::string &error) {
    error = "dictionary management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_delete_dictionary(const std::string &, DictionaryList &,
                                                    std::string &error) {
    error = "dictionary management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }
  virtual SettingsStatus settings_reload_dictionaries(DictionaryList &, std::string &error) {
    error = "dictionary management is not supported by this backend";
    return SettingsStatus::Unsupported;
  }

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
// Host backend options (Stage 6 WP3, owner decision U1). The standard Fcitx
// per-user path is enabled only for the product Host; every in-process test and
// fixture keeps the skipped path, because the pinned Fcitx has no user-directory
// override on Windows and a test must never touch real per-user data.
struct FcitxBackendOptions {
  bool user_path = false;
  // Fixture-only: bind Fcitx's own global "toggle input method" action to this key (for
  // example "F6") in the instance this backend starts, so a test can prove that a
  // configured global hotkey is handled with nothing composing. Empty keeps Fcitx's
  // configured key list. The product Host never sets it.
  std::string fixture_global_hotkey;
  // Fixture-only: replace the loaded group's input methods with this exact list and this
  // DefaultIM, so a test can reproduce a user's saved profile (for example a single
  // Shuangpin method) in its own instance. Empty keeps what the profile/first-run plan
  // decided. The product Host never sets it.
  std::vector<std::string> fixture_input_methods;
  std::string fixture_default_method;
  // Fixture-only: set Fcitx's own modifier-only "temporarily switch input method"
  // binding (Hotkey/AltTriggerKeys) to this key, for example "Shift_R", so a test can
  // prove that the configured key decides and not the shell. Empty keeps Fcitx's
  // configured value (Shift_L).
  std::string fixture_alt_trigger_key;
  // Fixture-only: start with this candidate-presentation preference ("Vertical",
  // "Horizontal" or "AutoScroll") instead of the schema's default, so a test can drive the
  // scrolling arrangement without writing a configuration file. Empty keeps the default.
  // The product Host never sets it: its value comes from the user's own configuration.
  std::string fixture_candidate_layout;
};
FUSION_FCITX_API std::unique_ptr<Backend> make_fcitx_backend(FcitxBackendOptions options = {});
} // namespace fusion
