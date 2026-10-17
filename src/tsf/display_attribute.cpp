#include "tsf/display_attribute.hpp"

#include "tsf/com_ptr.hpp"
#include "tsf/guids.hpp"

#include <oleauto.h>

#include <atomic>

namespace fusion::tsf {

namespace {

struct AttributeDef {
  const GUID *guid;
  const wchar_t *description;
  TF_DISPLAYATTRIBUTE value;
};

// Fcitx Underline: composition input text, underlined, app colors.
// Fcitx HighLight: the engine-highlighted part, drawn in the system selection
// colors (classic-ui renders HighLight with its highlight background).
const AttributeDef kAttributes[] = {
    {&GUID_FusionDisplayAttributeInput,
     L"Fusion composition input",
     {{TF_CT_NONE, {0}}, {TF_CT_NONE, {0}}, TF_LS_SOLID, FALSE, {TF_CT_NONE, {0}}, TF_ATTR_INPUT}},
    {&GUID_FusionDisplayAttributeHighlighted,
     L"Fusion composition highlight",
     {{TF_CT_SYSCOLOR, {COLOR_HIGHLIGHTTEXT}},
      {TF_CT_SYSCOLOR, {COLOR_HIGHLIGHT}},
      TF_LS_SOLID,
      FALSE,
      {TF_CT_NONE, {0}},
      TF_ATTR_TARGET_CONVERTED}},
};
constexpr ULONG kAttributeCount = static_cast<ULONG>(sizeof(kAttributes) / sizeof(kAttributes[0]));

class DisplayAttributeInfo final : public ITfDisplayAttributeInfo {
public:
  explicit DisplayAttributeInfo(const AttributeDef &def) : def_(&def) {}
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
      *ppv = static_cast<ITfDisplayAttributeInfo *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0)
      delete this;
    return count;
  }
  STDMETHODIMP GetGUID(GUID *pguid) override {
    if (pguid == nullptr)
      return E_INVALIDARG;
    *pguid = *def_->guid;
    return S_OK;
  }
  STDMETHODIMP GetDescription(BSTR *pbstrDesc) override {
    if (pbstrDesc == nullptr)
      return E_INVALIDARG;
    *pbstrDesc = SysAllocString(def_->description);
    return *pbstrDesc != nullptr ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE *pda) override {
    if (pda == nullptr)
      return E_INVALIDARG;
    *pda = def_->value;
    return S_OK;
  }
  // The attributes are fixed presentation of Fcitx format bits, not user styles.
  STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE *) override { return E_NOTIMPL; }
  STDMETHODIMP Reset() override { return S_OK; }

private:
  std::atomic<ULONG> ref_count_{1};
  const AttributeDef *def_;
};

class EnumDisplayAttributeInfo final : public IEnumTfDisplayAttributeInfo {
public:
  explicit EnumDisplayAttributeInfo(ULONG index = 0) : index_(index) {}
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr)
      return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
      *ppv = static_cast<IEnumTfDisplayAttributeInfo *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0)
      delete this;
    return count;
  }
  STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo **ppEnum) override {
    if (ppEnum == nullptr)
      return E_INVALIDARG;
    *ppEnum = new EnumDisplayAttributeInfo(index_);
    return S_OK;
  }
  STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo **rgInfo, ULONG *pcFetched) override {
    if (rgInfo == nullptr || (ulCount != 1 && pcFetched == nullptr))
      return E_INVALIDARG;
    ULONG fetched = 0;
    while (fetched < ulCount && index_ < kAttributeCount) {
      rgInfo[fetched++] = new DisplayAttributeInfo(kAttributes[index_++]);
    }
    if (pcFetched != nullptr)
      *pcFetched = fetched;
    return fetched == ulCount ? S_OK : S_FALSE;
  }
  STDMETHODIMP Reset() override {
    index_ = 0;
    return S_OK;
  }
  STDMETHODIMP Skip(ULONG ulCount) override {
    const ULONG left = kAttributeCount - index_;
    const ULONG step = ulCount < left ? ulCount : left;
    index_ += step;
    return step == ulCount ? S_OK : S_FALSE;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  ULONG index_;
};

ComRef<ITfRange> sub_range(ITfRange *composition, TfEditCookie ec, std::size_t start,
                           std::size_t end) {
  ComRef<ITfRange> range;
  if (FAILED(composition->Clone(range.out())) || FAILED(range->Collapse(ec, TF_ANCHOR_START)))
    return {};
  LONG moved = 0;
  if (end > 0 && (FAILED(range->ShiftEnd(ec, static_cast<LONG>(end), &moved, nullptr)) ||
                  moved != static_cast<LONG>(end)))
    return {};
  if (start > 0 && (FAILED(range->ShiftStart(ec, static_cast<LONG>(start), &moved, nullptr)) ||
                    moved != static_cast<LONG>(start)))
    return {};
  return range;
}

} // namespace

HRESULT create_display_attribute_enum(IEnumTfDisplayAttributeInfo **out) {
  if (out == nullptr)
    return E_INVALIDARG;
  *out = new EnumDisplayAttributeInfo();
  return S_OK;
}

HRESULT create_display_attribute_info(REFGUID guid, ITfDisplayAttributeInfo **out) {
  if (out == nullptr)
    return E_INVALIDARG;
  *out = nullptr;
  for (const auto &def : kAttributes) {
    if (IsEqualGUID(guid, *def.guid)) {
      *out = new DisplayAttributeInfo(def);
      return S_OK;
    }
  }
  return E_INVALIDARG;
}

DisplayAttributeAtoms register_display_attribute_atoms() {
  DisplayAttributeAtoms atoms;
  ITfCategoryMgr *raw = nullptr;
  if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITfCategoryMgr, reinterpret_cast<void **>(&raw))) ||
      raw == nullptr)
    return atoms;
  ComRef<ITfCategoryMgr> category(raw);
  if (FAILED(category->RegisterGUID(GUID_FusionDisplayAttributeInput, &atoms.input)))
    atoms.input = TF_INVALID_GUIDATOM;
  if (FAILED(category->RegisterGUID(GUID_FusionDisplayAttributeHighlighted, &atoms.highlighted)))
    atoms.highlighted = TF_INVALID_GUIDATOM;
  return atoms;
}

bool clear_display_attributes(ITfContext *context, ITfRange *range, TfEditCookie ec) {
  ComRef<ITfProperty> property;
  if (FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, property.out())) || !property)
    return false;
  return SUCCEEDED(property->Clear(ec, range));
}

bool apply_display_attributes(ITfContext *context, ITfRange *composition, TfEditCookie ec,
                              const PreeditPresentation &presentation,
                              const DisplayAttributeAtoms &atoms) {
  ComRef<ITfProperty> property;
  if (!atoms.valid() || FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, property.out())) ||
      !property)
    return false;
  if (FAILED(property->Clear(ec, composition)))
    return false;
  bool ok = true;
  for (const auto &styled : presentation.ranges) {
    if (styled.style == PreeditStyle::None || styled.end <= styled.start)
      continue;
    auto range = sub_range(composition, ec, styled.start, styled.end);
    if (!range) {
      ok = false;
      continue;
    }
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(styled.style == PreeditStyle::Highlighted ? atoms.highlighted
                                                                             : atoms.input);
    if (FAILED(property->SetValue(ec, range.get(), &value)))
      ok = false;
  }
  return ok;
}

bool place_caret_in_composition(ITfContext *context, ITfRange *composition, TfEditCookie ec,
                                std::size_t offset) {
  auto caret = sub_range(composition, ec, offset, offset);
  if (!caret)
    return false;
  TF_SELECTION selection{};
  selection.range = caret.get();
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  return SUCCEEDED(context->SetSelection(ec, 1, &selection));
}

} // namespace fusion::tsf
