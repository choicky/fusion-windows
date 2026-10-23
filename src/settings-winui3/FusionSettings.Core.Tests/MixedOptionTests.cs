using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.App.Services;
using Fusion.Settings.Client;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;

namespace Fusion.Settings.Tests;

/// <summary>
/// The Chinese presentation of the auxiliary filter, the English-candidate switch and the
/// mixed-input spacing switch, plus the stored values behind them.
/// </summary>
/// <remarks>
/// <para>
/// Two halves, because they fail differently. The <b>table</b> half needs no Host: it reads
/// the same <c>PresentationMap.cs</c> the window compiles and checks the labels, the group
/// each row lands in, and that every stored value of a mapped enumeration has its own label —
/// so a translation of the currently selected item alone cannot pass.
/// </para>
/// <para>
/// The <b>stored-value</b> half needs a real Host with a writable profile: it reads the
/// schema the Host publishes (the stored value names and the default), saves each auxiliary
/// filter value and reads it back, and does the same for the spacing switch. Nothing here
/// asserts what the window draws on screen; that is the app's ComboBox built from these same
/// two facts.
/// </para>
/// </remarks>
internal static class MixedOptionTests
{
    private const string Filter = "AuxiliaryFilter";
    private const string Spacing = "MixedAutoSpacing";
    private const string English = "SpellEnabled";

    /// <summary>The display table alone: labels, groups, and a label for every stored value.</summary>
    public static void RunTableChecks()
    {
        Harness.Section("mixed options: the Chinese table, without a Host");

        var filter = PresentationMap.FindPinyin(Filter);
        Harness.Check(filter is not null, "辅助筛选 has a placement");
        if (filter is not null)
        {
            Harness.CheckEqual(PresentationMap.PageCandidates, filter.Page,
                "it is on 候选与输入");
            Harness.CheckEqual("辅助筛选", filter.Section,
                "in a group named for it");
            Harness.CheckEqual("辅助筛选方式", filter.Label,
                "with the title the owner asked for");
            var labels = filter.EnumLabels;
            Harness.Check(labels is not null, "and a label for each of its stored values");
            if (labels is not null)
            {
                // The stored values are the Host's; only the displayed text is translated.
                Harness.CheckEqual("Disabled,MoQi,Stroke",
                    string.Join(",", labels.Keys.OrderBy(key => key, StringComparer.Ordinal)),
                    "keyed by exactly the three stored value names, unchanged");
                Harness.CheckEqual("关闭", labels["Disabled"], "Disabled reads 关闭");
                Harness.CheckEqual("笔画", labels["Stroke"], "Stroke reads 笔画");
                Harness.CheckEqual("墨奇", labels["MoQi"], "MoQi reads 墨奇");
                Harness.Check(labels.Values.Distinct(StringComparer.Ordinal).Count() == labels.Count,
                    "and no two values read the same");
            }

            // The trigger key keeps exactly one home: the keys page, in a group of the same
            // name, so the two halves of the feature are recognisably one feature.
            var trigger = PresentationMap.FindPinyin("FilterByStroke");
            Harness.Check(trigger is not null, "the trigger key is placed once");
            if (trigger is not null)
            {
                Harness.CheckEqual(PresentationMap.PageKeys, trigger.Page,
                    "on 按键, where keys are edited");
                Harness.CheckEqual("辅助筛选", trigger.Section, "in the same 辅助筛选 group");
                Harness.CheckEqual("辅助筛选触发键", trigger.Label, "as 辅助筛选触发键");
                Harness.Check(!string.Equals(trigger.Label, filter.Label, StringComparison.Ordinal),
                    "the key and the mode are two different rows, not one setting twice");
            }
        }

        var spacing = PresentationMap.FindPinyin(Spacing);
        Harness.Check(spacing is not null, "混输空格 has a placement");
        if (spacing is not null)
        {
            Harness.CheckEqual("混输候选自动加空格", spacing.Label, "with the suggested title");
            Harness.CheckEqual("英文候选与混输", spacing.Section,
                "next to the English-candidate switch");
            Harness.Check(spacing.EnumLabels is null, "it is a switch, so it has no value labels");
        }

        var english = PresentationMap.FindPinyin(English);
        Harness.Check(english is not null, "显示英文候选 keeps its entry");
        if (english is not null && spacing is not null)
        {
            Harness.CheckEqual("显示英文候选", english.Label, "under its existing Chinese name");
            Harness.CheckEqual("英文候选与混输", english.Section,
                "in the same group as the spacing switch");
            Harness.Check(english.Order < spacing.Order,
                "listed before the spacing switch");
            Harness.Check(PresentationMap.Pinyin.Values.Count(placement =>
                    string.Equals(placement.Label, english.Label, StringComparison.Ordinal)) == 1,
                "and it is the only row with that label — no second switch for the same setting");
        }

        Harness.Check(PresentationMap.CandidateSections.Contains("英文候选与混输", StringComparer.Ordinal),
            "候选与输入 declares the 英文候选与混输 group");
        Harness.Check(PresentationMap.CandidateSections.Contains("辅助筛选", StringComparer.Ordinal),
            "and the 辅助筛选 group");
        Harness.Check(PresentationMap.KeysSections.Contains("辅助筛选", StringComparer.Ordinal),
            "按键 declares 辅助筛选 too");

        // Every enumeration this round mapped has a label for every one of its stored values.
        // The values themselves are checked against the Host below; here the rule is that a
        // mapped enum is mapped entirely.
        foreach (var (name, placement) in PresentationMap.Pinyin.Concat(PresentationMap.Fuzzy)
                     .Where(pair => pair.Value.EnumLabels is not null)
                     .OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            var labels = placement.EnumLabels!;
            Harness.Check(labels.Count > 0 && labels.Values.All(text => !string.IsNullOrWhiteSpace(text)),
                name + ": every mapped value has a non-empty label");
        }

        // The keys of the enums whose stored strings are not their C++ identifiers. Written
        // out here because these are the ones a later editor is most likely to "tidy" back to
        // `ComposingPinyin`, which compiles, passes a label count, and shows English in the
        // window because the lookup never matches.
        foreach (var (name, stored) in new[]
                 {
                     ("PreeditMode", "Commit preview|Composing pinyin|Do not show"),
                     ("BackspaceBehaviorOnPrediction",
                         "Backspace when not using on-screen keyboard|Clear candidates & backspace|Only Clear candidates"),
                     ("SwitchInputMethodBehavior", "Clear|Commit current preedit|Commit default selection"),
                     ("AuxiliaryFilter", "Disabled|MoQi|Stroke"),
                     ("ShuangpinProfile", "ABC|Custom|GB|MS|PinyinJiajia|Xiaohe|Zhongwenzhixing|Ziguang|Ziranma"),
                 })
        {
            var labels = PresentationMap.FindPinyin(name)?.EnumLabels;
            Harness.Check(labels is not null, name + ": is mapped");
            if (labels is null)
            {
                continue;
            }

            Harness.CheckEqual(stored, string.Join("|", labels.Keys.OrderBy(key => key, StringComparer.Ordinal)),
                name + ": keyed by the strings the Host stores");
        }

        // No page shows two rows with the same Chinese title. Two configurations legitimately
        // carry same-named options (the engine's 上一页 and the core's fallback 上一页), and
        // they are two different settings; what must never happen is one title standing for
        // two rows, which is what a duplicated placement would look like on screen.
        foreach (var page in new[] { PresentationMap.PageCandidates, PresentationMap.PageKeys })
        {
            var titles = PresentationMap.Pinyin.Where(pair => pair.Value.Page == page)
                .Select(pair => pair.Value.Label)
                .Concat(PresentationMap.Presentation.Where(pair => pair.Value.Page == page)
                    .Select(pair => pair.Value.Label))
                .Concat(PresentationMap.Global.Where(pair => pair.Value.Page == page)
                    .Select(pair => pair.Value.Label))
                .ToList();
            var repeated = titles.GroupBy(title => title, StringComparer.Ordinal)
                .Where(group => group.Count() > 1)
                .Select(group => group.Key)
                .ToList();
            Harness.CheckEqual(string.Empty, string.Join(",", repeated),
                page + ": no two rows share one title");
        }
    }

    /// <summary>The same three settings against a real Host with a disposable profile.</summary>
    public static async Task RunAgainstHostAsync(string endpoint, CancellationToken cancellationToken)
    {
        Harness.Section("mixed options: stored values read, saved and reread");

        var workspace = new SettingsWorkspace(endpoint);
        Harness.Check(await workspace.InitializeAsync(cancellationToken).ConfigureAwait(false),
            "the workspace loads the Pinyin configuration");
        if (!workspace.IsLoaded)
        {
            Console.WriteLine($"   Host said: {workspace.StatusText}");
            return;
        }

        var snapshot = workspace.SnapshotFor(SettingsTarget.Pinyin);
        var draft = workspace.PinyinDraft;
        Harness.Check(snapshot is not null && draft is not null,
            "the Host describes the Pinyin engine configuration");
        if (snapshot is null || draft is null || snapshot.Types.Count == 0)
        {
            return;
        }

        var options = snapshot.Types[0].Options;
        OptionDescription? Option(string name) =>
            options.FirstOrDefault(candidate => string.Equals(candidate.Name, name, StringComparison.Ordinal));

        var filter = Option(Filter);
        Harness.Check(filter is not null, "the engine exposes " + Filter);
        if (filter is null)
        {
            return;
        }

        Harness.CheckEqual("Enum", filter.Type, "it is an enumeration");
        // The three stored values are the engine's own names. A display table that renamed
        // them would save a profile the engine cannot read, so they are asserted here.
        Harness.CheckEqual("Disabled|Stroke|MoQi", string.Join("|", filter.EnumLabels),
            "with exactly the three stored values the fork defines");
        Harness.CheckEqual("Stroke", filter.DefaultValue.Scalar,
            "and 笔画 as the default, as the engine declares it");

        var placement = PresentationMap.FindPinyin(Filter);
        Harness.Check(placement?.EnumLabels is not null, "the display table maps it");
        var shownLabels = placement?.EnumLabels ?? new Dictionary<string, string>();
        if (placement?.EnumLabels is { } labels)
        {
            var unmapped = filter.EnumLabels.Where(value => !labels.ContainsKey(value)).ToList();
            Harness.CheckEqual(string.Empty, string.Join(",", unmapped),
                "every stored value has a label, not only the selected one");
        }

        // Every enumeration this page shows has its whole value set translated, and the keys
        // are the strings the Host publishes. That second half is what a guess gets wrong: the
        // engine's stored value for a multi-word enum is its display string
        // (FCITX_CONFIG_ENUM_NAME_WITH_I18N), not the C++ identifier.
        void CheckEnumCoverage(OptionDescription option, Placement? table, string where)
        {
            if (table?.EnumLabels is null)
            {
                return;
            }

            var stored = option.EnumLabels.OrderBy(value => value, StringComparer.Ordinal).ToList();
            var mapped = table.EnumLabels.Keys.OrderBy(value => value, StringComparer.Ordinal).ToList();
            Harness.CheckEqual(string.Join("|", stored), string.Join("|", mapped),
                where + ": the labels are keyed by exactly the Host's stored values");
            Harness.CheckEqual(stored.Count,
                table.EnumLabels.Values.Distinct(StringComparer.Ordinal).Count(),
                where + ": every value reads differently");
        }

        foreach (var option in options.Where(option => option.Type == "Enum"))
        {
            CheckEnumCoverage(option, PresentationMap.FindPinyin(option.Name), option.Name);
        }

        // The nested fuzzy table holds one enumeration; it is on the same page, so it is held
        // to the same rule.
        var fuzzy = snapshot.Types.FirstOrDefault(type =>
            string.Equals(type.Name, "Fuzzy$FuzzyConfig", StringComparison.Ordinal));
        var correction = fuzzy?.Options.FirstOrDefault(option =>
            string.Equals(option.Name, "Correction", StringComparison.Ordinal));
        if (correction is not null)
        {
            CheckEnumCoverage(correction, PresentationMap.FindFuzzy("Correction"), "Fuzzy/Correction");
        }

        // The two enumerations the global configuration contributes to the same page
        // (PropertyPropagatePolicy: All / Program / No) follow the same rule.
        var global = workspace.SnapshotFor(SettingsTarget.Global);
        if (global is not null && global.Types.Count > 0)
        {
            var root = global.Types[0];
            foreach (var type in global.Types.Skip(1))
            {
                var container = root.Options.FirstOrDefault(option =>
                    string.Equals(option.Type, type.Name, StringComparison.Ordinal))?.Name;
                if (container is null)
                {
                    continue;
                }

                foreach (var option in type.Options.Where(option => option.Type == "Enum"))
                {
                    CheckEnumCoverage(option, PresentationMap.FindGlobal(container, option.Name),
                        container + "/" + option.Name);
                }
            }
        }

        // 墨奇 → read back → 关闭 → read back → back to the default.
        foreach (var (stored, shown) in new[]
                 {
                     ("MoQi", "墨奇"), ("Disabled", "关闭"), ("Stroke", "笔画"),
                 })
        {
            Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, Filter, SettingsValue.Text(stored)),
                "the draft takes " + stored);
            await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
            Console.WriteLine($"   {stored}: {workspace.SaveSummary.Replace("\n", " / ")}");
            Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
                "the Host accepts " + stored);
            Harness.CheckEqual(stored, await ReadPinyinValueAsync(endpoint, Filter, cancellationToken)
                    .ConfigureAwait(false),
                "and reports it back");
            // The label is display text only: what was written is the stored value above.
            Harness.CheckEqual(shown, shownLabels.TryGetValue(stored, out var text) ? text : string.Empty,
                stored + " is displayed as " + shown);
        }

        var spacing = Option(Spacing);
        Harness.Check(spacing is not null, "the engine exposes " + Spacing);
        if (spacing is null)
        {
            return;
        }

        Harness.CheckEqual("Boolean", spacing.Type, "which is a switch");
        Harness.CheckEqual("False", spacing.DefaultValue.Scalar, "off by default");
        Harness.CheckEqual("False", draft.Effective(Spacing).Scalar,
            "a profile that never set it reads as off");

        var englishBefore = draft.Effective(English).Scalar;
        Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, Spacing, SettingsValue.Text("True")),
            "the spacing switch is turned on");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.Check(!workspace.SaveSummary.Contains("未保存", StringComparison.Ordinal),
            "the Host accepts the switch");
        Harness.CheckEqual("True", await ReadPinyinValueAsync(endpoint, Spacing, cancellationToken)
                .ConfigureAwait(false),
            "and stores it on");
        Harness.CheckEqual(englishBefore,
            await ReadPinyinValueAsync(endpoint, English, cancellationToken).ConfigureAwait(false),
            "while " + English + " keeps its own stored value — one switch per concept");

        Harness.Check(workspace.TrySet(SettingsTarget.Pinyin, Spacing, SettingsValue.Text("False")),
            "and it is turned off again, the value this build ships");
        await workspace.SaveAsync(cancellationToken).ConfigureAwait(false);
        Harness.CheckEqual("False", await ReadPinyinValueAsync(endpoint, Spacing, cancellationToken)
                .ConfigureAwait(false),
            "the Host stores off");

        // There is no second switch for mixed input, in the schema or in the table: the
        // engine's own header states the decision, and this is what keeps it true.
        var mixedSwitches = options
            .Where(option => option.Name.Contains("Mixed", StringComparison.Ordinal) &&
                             !string.Equals(option.Name, Spacing, StringComparison.Ordinal))
            .Select(option => option.Name)
            .ToList();
        Harness.CheckEqual(string.Empty, string.Join(",", mixedSwitches),
            "the engine declares no other mixed-input switch");
    }

    private static async Task<string> ReadPinyinValueAsync(string endpoint, string name,
                                                           CancellationToken cancellationToken)
    {
        var reader = new SettingsClient(endpoint, TimeSpan.FromSeconds(10));
        var reread = await reader.GetConfigAsync(SettingsWorkspace.PinyinUri, cancellationToken)
            .ConfigureAwait(false);
        if (!reread.IsOk)
        {
            Console.WriteLine($"   reread failed: {SettingsWorkspace.Describe(reread.Status, reread.Error)}");
            return "(reread failed)";
        }

        return reread.Value!.Values.TryGet(name, out var value) ? value.Scalar : "(absent)";
    }
}
