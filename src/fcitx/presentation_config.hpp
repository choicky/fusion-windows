#pragma once

// Windows candidate-presentation configuration.
//
// Owner authorization (2026-10-22): a Windows-specific presentation setting may have its
// own configuration; official input semantics must not acquire duplicate stores. This is
// therefore one narrowly scoped configuration for how the Windows candidate window
// presents itself — today the layout direction only. It deliberately holds no engine or
// global option: Pinyin/Shuangpin behaviour stays in PinyinEngineConfig and input
// behaviour stays in GlobalConfig, each with exactly one store, and nothing here repeats
// a value those already own.
//
// It is a normal Fcitx configuration, not a second configuration system: an
// FCITX_CONFIGURATION schema described by Configuration::dumpDescription, stored through
// StandardPathsType::PkgConfig (the same per-user configuration directory every addon
// uses) and written with the same safe save the Host already applies to
// fcitx://config/global. It does not use classicui's namespace: that component is neither
// built nor loaded in this runtime, so a value stored there would have no reader.
//
// Scope stays narrow on purpose. No theme, no font, no colour and no candidate-window
// redesign — those are separate capabilities, not options of this one.

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-utils/i18n.h>

namespace fusion {

// The stored user preference. Two values only: "no choice" is the schema default rather
// than a third stored state, because a stored NotSet would be indistinguishable from an
// absent key, and "the user chose the default" and "the user never chose" would then be
// the same bytes describing different facts.
enum class CandidateLayoutPreference { Horizontal, Vertical };

FCITX_CONFIG_ENUM_NAME_WITH_I18N(CandidateLayoutPreference, N_("Horizontal"),
                                 N_("Vertical"))

// Default Vertical: a user who never opens the setting keeps the presentation this
// product shipped before the option existed, which is the requirement that outranks
// copying upstream's default. classicui's own default is the opposite (horizontal,
// classicui.h:129 at pinned fcitx5 a7f2528); that difference is deliberate and is
// recorded in the documentation rather than hidden.
FCITX_CONFIGURATION(
    CandidatePresentationConfig,
    fcitx::OptionWithAnnotation<CandidateLayoutPreference,
                                CandidateLayoutPreferenceI18NAnnotation> layout{
        this, "CandidateLayout", _("Candidate layout"),
        CandidateLayoutPreference::Vertical};);

// Path inside Fcitx's per-user configuration directory (StandardPathsType::PkgConfig).
// A file of its own, so the value is never mixed into a configuration another component
// owns and never disappears when that component rewrites its schema.
inline constexpr char candidate_presentation_config_path[] =
    "conf/candidate-presentation.conf";

} // namespace fusion
