using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Controls;
using Fusion.Settings.App.Services;
using Fusion.Settings.Workspace;
using Fusion.Settings.Model;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App.Views;

/// <summary>
/// Common base: pages rebuild from the workspace rather than holding their own copy of the
/// configuration. That is what makes navigation preserve unsaved edits — the draft never
/// lives in a page.
/// </summary>
public abstract class SettingsPageBase : Page
{
    protected SettingsWorkspace Workspace { get; private set; } = null!;

    /// <summary>Binds the page to the workspace and builds its content.</summary>
    public void Initialize(SettingsWorkspace workspace)
    {
        Workspace = workspace;
        Services.Trace.Write("page build " + GetType().Name);
        Content = BuildContent();
        Services.Trace.Write("page built " + GetType().Name);
    }

    /// <summary>Rebuilds after a save, discard or method switch.</summary>
    public void Refresh()
    {
        Services.Trace.Write("page refresh " + GetType().Name);
        Content = BuildContent();
        Services.Trace.Write("page refreshed " + GetType().Name);
    }

    protected abstract UIElement BuildContent();

    /// <summary>
    /// Rows for the current Pinyin/Shuangpin view that belong on <paramref name="page"/>.
    /// </summary>
    /// <remarks>
    /// The page filter is what stops every page rendering every option: the presentation
    /// table decides where a row goes, and an option it does not place falls back to one
    /// deliberate location rather than appearing everywhere. The setter is bound to the target
    /// this page renders, so a row can only ever write to its own configuration.
    /// </remarks>
    protected static IEnumerable<(string Section, int Order, FrameworkElement Row)> PinyinRows(
        SettingsWorkspace workspace, string page)
    {
        var target = workspace.CurrentTarget;
        var snapshot = workspace.SnapshotFor(target);
        var draft = workspace.DraftFor(target);
        if (snapshot is null || draft is null || snapshot.Types.Count == 0)
        {
            yield break;
        }

        // Every key list in this view, so a row can say when a chord it holds is already used
        // by another action. It reports; it never rewrites either list.
        var keyLists = snapshot.Types[0].Options
            .Where(candidate => candidate.IsKeyList)
            .ToDictionary(candidate => candidate.Name,
                          candidate => KeyBindingList.FromValue(draft.Effective(candidate.Name)),
                          StringComparer.Ordinal);

        foreach (var option in snapshot.Types[0].Options)
        {
            if (option.IsExternal || option.IsNested)
            {
                continue;
            }

            var placement = PresentationMap.FindPinyin(option.Name);
            var optionPage = placement?.Page ?? PresentationMap.PageCandidates;
            if (!string.Equals(optionPage, page, StringComparison.Ordinal))
            {
                continue;
            }

            Func<KeyChord, string?>? probe = null;
            if (option.IsKeyList)
            {
                var selfName = option.Name;
                probe = chord =>
                {
                    foreach (var (name, list) in keyLists)
                    {
                        if (!string.Equals(name, selfName, StringComparison.Ordinal) && list.Contains(chord))
                        {
                            var other = PresentationMap.FindPinyin(name)?.Label ?? name;
                            return $"与「{other}」使用了同一个按键。";
                        }
                    }

                    return null;
                };
            }

            var row = OptionRows.Build(option, draft.Effective(option.Name),
                value => workspace.TrySet(target, option.Name, value),
                placement?.Label, placement?.Help, probe);

            yield return (placement?.Section ?? "更多设置", placement?.Order ?? 900, row);
        }
    }

    /// <summary>
    /// Rows for the global configuration's nested Hotkey/Behavior containers on
    /// <paramref name="page"/>. The target is fixed to Global, which is the defect this
    /// replaces: these rows used to be applied to the Pinyin draft.
    /// </summary>
    protected static IEnumerable<(string Section, int Order, FrameworkElement Row)> GlobalRows(
        SettingsWorkspace workspace, string page)
    {
        var snapshot = workspace.SnapshotFor(SettingsTarget.Global);
        var draft = workspace.DraftFor(SettingsTarget.Global);
        if (snapshot is null || draft is null || snapshot.Types.Count == 0)
        {
            yield break;
        }

        var root = snapshot.Types[0];
        foreach (var type in snapshot.Types.Skip(1))
        {
            // The value-tree key is the root option whose declared type is this container.
            var container = root.Options.FirstOrDefault(option =>
                string.Equals(option.Type, type.Name, StringComparison.Ordinal))?.Name;
            if (container is null)
            {
                continue;
            }

            foreach (var option in type.Options)
            {
                var placement = PresentationMap.FindGlobal(container, option.Name);
                var optionPage = placement?.Page ?? PresentationMap.PageCandidates;
                if (!string.Equals(optionPage, page, StringComparison.Ordinal))
                {
                    continue;
                }

                var row = OptionRows.Build(
                    option,
                    draft.EffectiveNested(snapshot, type.Name, option.Name),
                    value => workspace.TrySetNested(SettingsTarget.Global, type.Name, option.Name, value),
                    placement?.Label,
                    placement?.Help);

                yield return (placement?.Section ?? "更多设置", placement?.Order ?? 900, row);
            }
        }
    }

    /// <summary>Groups rows into sections in the order the presentation table declares.</summary>
    protected static void AddGroupedSections(StackPanel body, IEnumerable<(string Section, int Order, FrameworkElement Row)> rows,
                                             IReadOnlyList<string> sectionOrder)
    {
        var groups = rows
            .GroupBy(entry => entry.Section)
            .OrderBy(group => IndexOf(sectionOrder, group.Key))
            .ToList();

        foreach (var group in groups)
        {
            PageChrome.AddSection(body, group.Key, null,
                group.OrderBy(entry => entry.Order).Select(entry => entry.Row));
        }
    }

    private static int IndexOf(IReadOnlyList<string> order, string section)
    {
        for (var i = 0; i < order.Count; i++)
        {
            if (string.Equals(order[i], section, StringComparison.Ordinal))
            {
                return i;
            }
        }

        return order.Count;
    }

    protected static string MethodLabel(string name) => name switch
    {
        "pinyin" => "拼音",
        "shuangpin" => "双拼",
        _ => name,
    };
}
