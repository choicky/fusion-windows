using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Fusion.Settings.App.Services;
using Fusion.Settings.Workspace;
using Fusion.Settings.Model;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App.Views;

/// <summary>
/// Every key binding, grouped by what the keys do. The engine's paging keys and the
/// system's fallback paging keys are two different options and are shown as such.
/// </summary>
public sealed class KeysPage : SettingsPageBase
{
    protected override UIElement BuildContent()
    {
        var page = PageChrome.Create(
            "按键",
            "为每个动作设置一组可选按键。一个按键组合就是一个标签。",
            out var body);

        body.Children.Add(PageChrome.MethodSelector(Workspace, Refresh));

        var rows = PinyinRows(Workspace, PresentationMap.PageKeys).ToList();

        // The paging preset edits the same official PrevPage/NextPage lists, so it lives in
        // that section rather than inventing a separate paging switch.
        rows.Add(("翻页", 900, BuildPagingPreset()));

        AddGroupedSections(body, rows, PresentationMap.KeysSections);

        var globalRows = GlobalRows(Workspace, PresentationMap.PageKeys).ToList();
        AddGroupedSections(body, globalRows, PresentationMap.KeysSections);

        if (globalRows.Count > 0)
        {
            body.Children.Add(PageChrome.Note(
                "系统快捷键由 TSF 路径送达。带 Ctrl/Alt/Win 的组合键与纯修饰键在此路径上不会被送达，" +
                "因此这些绑定可以保存但不会触发；只有未修饰的按键会生效。"));
        }

        return page;
    }

    /// <summary>
    /// Comma for the previous page and period for the next, applied to the official lists.
    /// Replacing and extending are different actions, and the dialog says which one it is
    /// before anything changes.
    /// </summary>
    private FrameworkElement BuildPagingPreset()
    {
        var panel = new StackPanel { Spacing = 8 };

        panel.Children.Add(new TextBlock
        {
            Text = "使用 ，和 。 翻页",
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
        });

        panel.Children.Add(new TextBlock
        {
            Text = "把「，」设为上一页、「。」设为下一页。这会修改上面两个官方列表本身，不会新增开关。",
            Opacity = 0.7,
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        });

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };

        var extend = new Button { Content = "追加到现有绑定", Padding = new Thickness(12, 5, 12, 5) };
        extend.Click += async (_, _) => await ApplyPresetAsync(replace: false);

        var replace = new Button { Content = "替换现有绑定", Padding = new Thickness(12, 5, 12, 5) };
        replace.Click += async (_, _) => await ApplyPresetAsync(replace: true);

        buttons.Children.Add(extend);
        buttons.Children.Add(replace);
        panel.Children.Add(buttons);
        return panel;
    }

    private async Task ApplyPresetAsync(bool replace)
    {
        var snapshot = Workspace.SnapshotFor(Workspace.CurrentTarget);
        var draft = Workspace.DraftFor(Workspace.CurrentTarget);
        if (snapshot is null || draft is null)
        {
            return;
        }

        var confirm = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = replace ? "替换现有的翻页绑定？" : "追加翻页绑定？",
            Content = replace
                ? "现有的「上一页」和「下一页」绑定会被清除，只保留「，」和「。」。"
                : "「，」和「。」会加到现有的「上一页」和「下一页」绑定之后，已有绑定保留。",
            PrimaryButtonText = "继续",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await confirm.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        ApplyPresetTo("PrevPage", "comma", replace);
        ApplyPresetTo("NextPage", "period", replace);
        Refresh();
    }

    private void ApplyPresetTo(string optionName, string keysym, bool replace)
    {
        var snapshot = Workspace.SnapshotFor(Workspace.CurrentTarget);
        var draft = Workspace.DraftFor(Workspace.CurrentTarget);
        if (snapshot is null || draft is null)
        {
            return;
        }

        var list = KeyBindingList.FromValue(draft.Effective(optionName));
        var updated = replace ? KeyBindingList.Empty() : list.Clone();
        var chord = KeyChord.Parse(keysym);
        if (chord is not null && (replace || !updated.Contains(chord)))
        {
            updated.Add(chord);
        }

        Workspace.TrySet(Workspace.CurrentTarget, optionName, updated.ToValue());
    }
}
