#pragma once
#include "fusion/core.hpp"
#include <cstdint>
namespace fusion::tsf {
inline bool async_admitted(std::int32_t outer, std::int32_t inner) {
  return outer >= 0 && inner == 0x00040300; // SDK TF_S_ASYNC: admission only
}
inline EditResult observed_edit(bool mutated, bool readback, bool composition_ended) {
  if (!mutated)
    return EditResult::RefusedNoMutation;
  return readback && composition_ended ? EditResult::AppliedReadback : EditResult::Indeterminate;
}
inline bool same_entry_generation(std::uint64_t captured, std::uint64_t live) {
  return captured == live;
}
} // namespace fusion::tsf
