using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Controls;
using Fusion.Settings.App.Services;
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

    /// <summary>Rows for the current Pinyin/Shuangpin view, grouped by the presentation table.</summary>
    protected static IEnumerable<(string Section, int Order, FrameworkElement Row)> PinyinRows(
        SettingsWorkspace workspace)
    {
        var snapshot = workspace.SnapshotFor(workspace.CurrentTarget);
        var draft = workspace.DraftFor(workspace.CurrentTarget);
        if (snapshot is null || draft is null || snapshot.Types.Count == 0)
        {
            yield break;
        }

        // The root type is the engine configuration; nested types are rendered by the page
        // that owns them, and External options are links rather than values.
        foreach (var option in snapshot.Types[0].Options)
        {
            if (option.IsExternal || option.IsNested)
            {
                continue;
            }

            var placement = PresentationMap.FindPinyin(option.Name);
            var section = placement?.Section ?? "更多设置";
            var order = placement?.Order ?? 900;

            var row = OptionRows.Build(option, draft.Effective(option.Name),
                value => workspace.TrySet(option.Name, value),
                placement?.Label, placement?.Help);

            yield return (section, order, row);
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
