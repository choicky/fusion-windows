using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Linq;
using Fusion.Settings.App.Services;
using Fusion.Settings.Model;
using Fusion.Settings.Workspace;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

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

        // The candidate window's own presentation comes first: it governs how the list is
        // drawn, before anything about which candidates are produced.
        AddGroupedSections(body, PresentationRows(Workspace, PresentationMap.PageCandidates),
            PresentationMap.CandidateSections);
        AddCandidatePreview(body);

        AddGroupedSections(body, PinyinRows(Workspace, PresentationMap.PageCandidates),
            PresentationMap.CandidateSections);

        // The global behaviour options belong to this page; the global hotkeys belong to Keys.
        AddGroupedSections(body, GlobalRows(Workspace, PresentationMap.PageCandidates),
            PresentationMap.CandidateSections);
        AddFuzzySection(body);
        AddExternalTools(body);

        return page;
    }

    /// <summary>
    /// A preview of the candidate window's own typography, plus the one action that belongs
    /// to it: put the font back to the Host schema's defaults.
    ///
    /// The preview reads the draft, so it shows what Apply would store, and it changes when
    /// a font row changes. It is sample text, not a candidate: it never touches the engine,
    /// the candidate list or the selection, and the defaults it restores are the schema's
    /// own DefaultValue rather than a number copied into this file.
    /// </summary>
    private void AddCandidatePreview(StackPanel body)
    {
        var snapshot = Workspace.SnapshotFor(SettingsTarget.Presentation);
        var draft = Workspace.DraftFor(SettingsTarget.Presentation);
        if (snapshot is null || draft is null || snapshot.Types.Count == 0)
        {
            return;
        }

        var sample = new TextBlock
        {
            Text = "1. 你好  2. 尼  3. 泥  4. 拟  —  nǐ hǎo",
            TextWrapping = TextWrapping.NoWrap,
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        var panel = new Border
        {
            Padding = new Thickness(12, 8, 12, 8),
            CornerRadius = new CornerRadius(4),
            HorizontalAlignment = HorizontalAlignment.Left,
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            BorderThickness = new Thickness(1),
            Child = sample,
        };

        void Render()
        {
            var family = draft.Effective("FontFamily").Scalar;
            var sizeText = draft.Effective("FontSize").Scalar;
            if (!string.IsNullOrWhiteSpace(family))
            {
                sample.FontFamily = new FontFamily(family);
            }

            // The stored value is points; WinUI measures in DIPs. Converting here keeps the
            // preview the same physical size as the popup, which scales points by the
            // monitor DPI for the same reason.
            if (double.TryParse(sizeText, NumberStyles.Float, CultureInfo.InvariantCulture, out var points))
            {
                sample.FontSize = Math.Clamp(points * 96.0 / 72.0, 6.0, 96.0);
            }
        }

        Render();

        var reset = new Button { Content = "恢复默认字体" };
        reset.Click += (_, _) =>
        {
            foreach (var name in new[] { "FontFamily", "FontSize" })
            {
                var option = snapshot.Types[0].Options.FirstOrDefault(candidate =>
                    string.Equals(candidate.Name, name, StringComparison.Ordinal));
                if (option is not null)
                {
                    Workspace.TrySet(SettingsTarget.Presentation, name, option.DefaultValue);
                }
            }

            // The FontFamily TextBox and FontSize NumberBox are the presentation rows built
            // above, and each one snapshots the draft value when it is created — it has no
            // live link back to the workspace. Writing the default into the draft alone would
            // leave those two editors showing the text the user last typed, which is why a
            // preview-only Render() was not enough. Rebuild the page (the same thing the
            // method selector does): the rows re-read draft.Effective, so both editors now show
            // the restored default, and the preview is rebuilt from it too.
            Refresh();
        };

        void OnWorkspace(object? sender, PropertyChangedEventArgs args)
        {
            if (string.Equals(args.PropertyName, nameof(SettingsWorkspace.HasChanges), StringComparison.Ordinal))
            {
                Render();
            }
        }

        // Follow the draft while this page is on screen, and stop when it is replaced: the
        // workspace outlives every page, so a handler left behind would keep the page alive.
        Workspace.PropertyChanged += OnWorkspace;
        panel.Unloaded += (_, _) => Workspace.PropertyChanged -= OnWorkspace;

        var rows = new List<FrameworkElement> { panel, reset };
        PageChrome.AddSection(body, "候选窗口预览",
            "这里显示候选窗口实际使用的字体和字号；改动候选字体或字号后即时生效，点击保存后应用到候选窗口。",
            rows);
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
            // Only the enumeration in this container is placed; the toggles fall back to the
            // Host's own labels, which is what this page shows for them today.
            var placement = PresentationMap.FindFuzzy(option.Name);
            rows.Add(Controls.OptionRows.Build(
                option,
                draft.EffectiveNested(snapshot, fuzzyType.Name, option.Name),
                value => Workspace.TrySetNested(Workspace.CurrentTarget, fuzzyType.Name, option.Name, value),
                placement?.Label, placement?.Help, enumLabels: placement?.EnumLabels));
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
