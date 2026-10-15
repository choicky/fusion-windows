#pragma once
#include "fusion/core.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace fusion {
// POC-LOCAL / NOT ACCEPTED ARCHITECTURE: an in-process Host adapter interface,
// not a final wire schema, ownership policy, or D072 P0 resolution.
struct BackendStimulus {
  enum class Type { Character, Space, Backspace, Escape, Return };
  Binding binding{};
  std::uint64_t stimulus_id = 0;
  Type type = Type::Character;
  char character = 0;
};
struct BackendOperation {
  enum class Type { SetPreedit, CandidateSnapshot, CommitText, ForwardKey };
  Type type = Type::SetPreedit;
  std::string text;
  std::vector<std::string> candidates;
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
