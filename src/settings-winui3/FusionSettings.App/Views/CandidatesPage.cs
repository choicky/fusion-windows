using System;
using System.Collections.Generic;
using System.Linq;
using Fusion.Settings.App.Services;
using Fusion.Settings.Model;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App.Views;

/// <summary>
/// Candidate, preedit, learning and fuzzy-pinyin behaviour for the selected method.
/// </summary>
public sealed class CandidatesPage : SettingsPageBase
{
    protected override UIElement BuildContent()
    {
        var page = PageChrome.Create(
            "候选与输入",
            "候选如何产生、预编辑如何显示，以及学习与模糊音。",
            out var body);

        body.Children.Add(PageChrome.MethodSelector(Workspace, Refresh));

        AddGroupedSections(body, PinyinRows(Workspace), PresentationMap.CandidateSections);
        AddFuzzySection(body);
        AddExternalTools(body);

        return page;
    }

    /// <summary>
    /// The nested <c>Fuzzy</c> configuration. It is one container in the schema, so it is
    /// one section here rather than twenty-one top-level rows.
    /// </summary>
    private void AddFuzzySection(StackPanel body)
    {
        var snapshot = Workspace.SnapshotFor(Workspace.CurrentTarget);
        var draft = Workspace.DraftFor(Workspace.CurrentTarget);
        if (snapshot is null || draft is null)
        {
            return;
        }

        var fuzzyType = snapshot.Types.FirstOrDefault(type =>
            string.Equals(type.Name, "Fuzzy$FuzzyConfig", StringComparison.Ordinal));
        if (fuzzyType is null)
        {
            return;
        }

        var rows = new List<FrameworkElement>();
        foreach (var option in fuzzyType.Options)
        {
            rows.Add(Controls.OptionRows.Build(
                option,
                draft.EffectiveNested(snapshot, fuzzyType.Name, option.Name),
                value => Workspace.TrySetNested(fuzzyType.Name, option.Name, value)));
        }

        PageChrome.AddSection(body, "模糊音",
            "允许把相近的拼音视为相同，用于容忍口音或输入习惯。默认只开启最常见的几项。",
            rows);
    }

    /// <summary>
    /// The External options are links to tools this build does not ship. They are listed
    /// with the reason rather than hidden, so a reader can see the capability exists
    /// upstream and is not silently missing.
    /// </summary>
    private void AddExternalTools(StackPanel body)
    {
        var snapshot = Workspace.SnapshotFor(Workspace.CurrentTarget);
        if (snapshot is null || snapshot.Types.Count == 0)
        {
            return;
        }

        var rows = new List<FrameworkElement>();
        foreach (var option in snapshot.Types[0].Options.Where(option => option.IsExternal))
        {
            var label = PresentationMap.ExternalToolReason.TryGetValue(option.Name, out var reason)
                ? reason
                : "该工具尚未在 Windows 上提供。";

            var stack = new StackPanel { Spacing = 2 };
            stack.Children.Add(new TextBlock
            {
                Text = option.Name,
                Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            });
            stack.Children.Add(new TextBlock
            {
                Text = label,
                Opacity = 0.7,
                TextWrapping = TextWrapping.Wrap,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
            rows.Add(stack);
        }

        PageChrome.AddSection(body, "外部工具",
            "这些是 Fcitx 上游的独立工具页面。本安装包没有提供它们，因此这里只说明状态，不提供入口。",
            rows);
    }
}
