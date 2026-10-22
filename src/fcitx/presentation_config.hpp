#pragma once

// Windows candidate-presentation configuration.
//
// Owner authorization (2026-10-22): a Windows-specific presentation setting may have its
// own configuration; official input semantics must not acquire duplicate stores. This is
// therefore one narrowly scoped configuration for how the Windows candidate window
// presents itself — the layout direction, the candidate font and its size. It deliberately
// holds no engine or global option: Pinyin/Shuangpin behaviour stays in
// PinyinEngineConfig and input behaviour stays in GlobalConfig, each with exactly one
// store, and nothing here repeats a value those already own.
//
// It is a normal Fcitx configuration, not a second configuration system: an
// FCITX_CONFIGURATION schema described by Configuration::dumpDescription, stored through
// StandardPathsType::PkgConfig (the same per-user configuration directory every addon
// uses) and written with the same safe save the Host already applies to
// fcitx://config/global. It does not use classicui's namespace: that component is neither
// built nor loaded in this runtime, so a value stored there would have no reader.
//
// Scope stays narrow on purpose. No theme, no colour and no candidate-window redesign —
// those are separate capabilities, not options of this one.

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-utils/i18n.h>

#include <string>

namespace fusion {

// The stored user preference. "No choice" is the schema default rather than a stored
// state, because a stored NotSet would be indistinguishable from an absent key, and "the
// user chose the default" and "the user never chose" would then be the same bytes
// describing different facts.
//
// The stored names are ASCII and stable; the Chinese labels the settings window shows are
// supplied by the settings client, so a stored value never depends on the display
// language. CandidateLayout::NotSet stays reserved for "the engine's own list hint", and
// this enum has no such value: the projection resolves the preference before the page is
// sent (see candidate_layout_for).
//
// AutoScroll is a third *preference*, and deliberately not a third CandidateLayout: the
// layout enum mirrors fcitx::CandidateLayoutHint, which has no scrolling value. AutoScroll
// means "one row per engine page, up to five of them at once, following the paging keys" —
// the arrangement is presentation (CandidateArrangement::Scroll) and each row is still
// laid out as a row. An engine hint still wins over this, exactly as it wins over the
// other two (resolve_candidate_layout), so a list that asks for a column gets a column.
enum class CandidateLayoutPreference { Horizontal, Vertical, AutoScroll };

FCITX_CONFIG_ENUM_NAME_WITH_I18N(CandidateLayoutPreference, N_("Horizontal"),
                                 N_("Vertical"), N_("AutoScroll"))

// Default Vertical: a user who never opens the setting keeps the presentation this
// product shipped before the option existed, which is the requirement that outranks
// copying upstream's default. classicui's own default is the opposite (horizontal,
// classicui.h:129 at pinned fcitx5 a7f2528); that difference is deliberate and is
// recorded in the documentation rather than hidden.
//
// The font is the one the Windows presentation already used before it was configurable
// (Microsoft YaHei UI at 15 device pixels on a 100 % display), expressed here in points so
// the stored value is independent of the display: 11 pt is 14.67 px at 96 dpi, i.e. what
// that pixel height was. The size is a logical unit (points), not a device unit, because
// the popup is drawn on whichever monitor the caret is on and the same stored value has to
// mean the same thing there.
FCITX_CONFIGURATION(
    CandidatePresentationConfig,
    fcitx::OptionWithAnnotation<CandidateLayoutPreference,
                                CandidateLayoutPreferenceI18NAnnotation> layout{
        this, "CandidateLayout", _("Candidate layout"),
        CandidateLayoutPreference::Vertical};
    fcitx::Option<std::string> font_family{this, "FontFamily", _("Candidate font"),
                                           "Microsoft YaHei UI"};
    fcitx::Option<int, fcitx::IntConstrain> font_size{this, "FontSize",
                                                      _("Candidate font size (points)"), 11,
                                                      fcitx::IntConstrain(6, 48)};);

// The size bounds and default again, for the code that has to name them without the
// schema: the presentation's own fallback when a page carries no font, and the popup's
// sanity clamp. The settings client's "restore default" action reads the schema's own
// DefaultValue from the Host instead of copying these, so there is still one stored
// authority — this is a convenience for code inside the Host, not a second one.
inline constexpr int candidate_font_size_default_pt = 11;
inline constexpr int candidate_font_size_min_pt = 6;
inline constexpr int candidate_font_size_max_pt = 48;

// Path inside Fcitx's per-user configuration directory (StandardPathsType::PkgConfig).
// A file of its own, so the value is never mixed into a configuration another component
// owns and never disappears when that component rewrites its schema.
inline constexpr char candidate_presentation_config_path[] =
    "conf/candidate-presentation.conf";

} // namespace fusion
