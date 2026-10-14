#include "mock_backend.hpp"

namespace fusion::test {

namespace {

bool starts_with(const std::string &input, const std::string &prefix) {
  return input.size() >= prefix.size() && input.compare(0, prefix.size(), prefix) == 0;
}

std::string payload(const std::string &input, const std::string &prefix) {
  return input.substr(prefix.size());
}

} // namespace

Response MockBackend::respond(const std::string &input) const {
  if (starts_with(input, "preedit:")) {
    return {Intent::Preedit, payload(input, "preedit:"), session_};
  }
  if (starts_with(input, "commit:")) {
    return {Intent::Commit, payload(input, "commit:"), session_};
  }
  if (starts_with(input, "reject:")) {
    return {Intent::Reject, payload(input, "reject:"), session_};
  }
  return {Intent::Pass, std::string(), session_};
}

} // namespace fusion::test
