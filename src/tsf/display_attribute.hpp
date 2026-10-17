#pragma once

// Stage 5 TSF display attributes for the composition: the lawful TSF carrier of
// the Fcitx preedit format (see preedit_presentation.hpp). The text service is
// an ITfDisplayAttributeProvider for exactly two attributes — composition input
// (Fcitx Underline) and highlighted composition (Fcitx HighLight) — and tags
// composition ranges through GUID_PROP_ATTRIBUTE. Presentation only: a failure
// here never changes what the composition text is.

#include "tsf/preedit_presentation.hpp"

#include <windows.h>

#include <msctf.h>

namespace fusion::tsf {

// Class objects handed out by ITfDisplayAttributeProvider. Both return
// E_INVALIDARG for an unknown GUID / null out pointer.
HRESULT create_display_attribute_enum(IEnumTfDisplayAttributeInfo **out);
HRESULT create_display_attribute_info(REFGUID guid, ITfDisplayAttributeInfo **out);

// Guid atoms registered with the category manager for the two attributes.
struct DisplayAttributeAtoms {
  TfGuidAtom input = TF_INVALID_GUIDATOM;
  TfGuidAtom highlighted = TF_INVALID_GUIDATOM;
  bool valid() const { return input != TF_INVALID_GUIDATOM && highlighted != TF_INVALID_GUIDATOM; }
};
DisplayAttributeAtoms register_display_attribute_atoms();

// Replace the attribute property over the whole composition range with the
// styled ranges of `presentation` (UTF-16 offsets from the range start).
bool apply_display_attributes(ITfContext *context, ITfRange *composition, TfEditCookie ec,
                              const PreeditPresentation &presentation,
                              const DisplayAttributeAtoms &atoms);

// Remove every attribute value from `range` (committed or abandoned text keeps
// no composition styling).
bool clear_display_attributes(ITfContext *context, ITfRange *range, TfEditCookie ec);

// Collapse to the start of `composition` and shift by `offset` UTF-16 units.
bool place_caret_in_composition(ITfContext *context, ITfRange *composition, TfEditCookie ec,
                                std::size_t offset);

} // namespace fusion::tsf
