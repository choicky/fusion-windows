using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;
using Fusion.Settings.Model;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;

namespace Fusion.Settings.App.Views;

/// <summary>
/// The pinyin dictionaries the engine loads: list, import, enable, disable, delete, reload.
/// </summary>
/// <remarks>
/// <para>
/// This page owns no state beyond the list the Host last reported. Enable is the engine's
/// own `<file>.disable` marker and the list comes from the two data roots the engine
/// searches, so there is nothing here to keep in step: every action's reply carries the
/// resulting list, and 重新加载 re-reads it.
/// </para>
/// <para>
/// An operation that the Host refuses because a composition is live is reported as such and
/// nothing is retried automatically — the same rule the rest of the window follows.
/// </para>
/// </remarks>
public sealed class DictionariesPage : SettingsPageBase
{
    private const string SupportedTypes = "拼音词库：.dict（LibIME 二进制）或 .txt（文本词库）";

    private SettingsClient _client = null!;

    // Built with the page, not once for the object: Refresh() replaces Content by calling
    // BuildContent() again, and a control can only have one parent. A status line and a row
    // container kept across rebuilds were therefore added to a second panel while the old
    // page still owned them, which is the "element is already the child of another element"
    // fault the window's 重新加载 button produced on this page.
    private TextBlock _status = null!;
    private StackPanel _rows = null!;
    private Button? _importButton;
    private Button? _reloadButton;
    private bool _busy;

    protected override UIElement BuildContent()
    {
        _status = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Opacity = 0.75,
        };
        _rows = new StackPanel { Spacing = 12 };

        // A conversion reads and rewrites a whole dictionary, so the default five seconds of
        // a settings request is not enough for an import. The workspace is bound before the
        // page is built, which is when its endpoint is known.
        _client ??= new SettingsClient(Workspace.Endpoint, TimeSpan.FromSeconds(60));
        var page = PageChrome.Create(
            "词库管理",
            "导入、启用、停用和删除拼音词库。词库由 Fcitx 的拼音引擎自己加载，这里只负责文件与启用状态。",
            out var body);

        var toolbar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        _importButton = new Button { Content = "导入词库…" };
        _importButton.Click += async (_, _) => await ImportAsync();
        _reloadButton = new Button { Content = "重新加载" };
        _reloadButton.Click += async (_, _) =>
            await RunAsync(client => client.ReloadDictionariesAsync(CancellationToken.None));
        toolbar.Children.Add(_importButton);
        toolbar.Children.Add(_reloadButton);

        PageChrome.AddSection(body, "词库", null, new FrameworkElement[] { toolbar, _status, _rows });

        PageChrome.AddSection(body, "说明", null, new FrameworkElement[]
        {
            PageChrome.Note(SupportedTypes + "。导入时先转换并校验，成功后才放进用户词库目录；"
                            + "失败不会改动已有的词库。同名词库会让你选择覆盖或跳过。"),
            PageChrome.Note("启用与停用使用 Fcitx 自己的同名 .disable 标记，没有第二份状态。"
                            + "随包提供的词库可以停用，但不能删除。"),
            PageChrome.Note("搜狗 .scel 词库暂不支持：它需要 addon 的 scel2org5 转换工具，"
                            + "而当前 Windows 运行时没有构建该工具。"),
            PageChrome.Note("拼音与双拼使用同一批词库，这里只有一份。删除不会触碰 user.dict、学习记录或系统主词库。"),
        });

        _ = RefreshAsync();
        return page;
    }

    private async Task RefreshAsync()
    {
        await RunAsync(client => client.ListDictionariesAsync(default));
    }

    private async Task ImportAsync()
    {
        var window = (Application.Current as App)?.MainWindow;
        if (window is null)
        {
            return;
        }

        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".dict");
        picker.FileTypeFilter.Add(".txt");
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(window));

        var files = await picker.PickMultipleFilesAsync();
        if (files is null || files.Count == 0)
        {
            return;
        }

        foreach (var file in files)
        {
            await ImportOneAsync(file.Path, System.IO.Path.GetFileName(file.Path));
        }
    }

    private async Task ImportOneAsync(string path, string fileName)
    {
        if (string.IsNullOrEmpty(path))
        {
            Report(fileName + "：这个来源没有本机路径，无法导入。");
            return;
        }

        SetBusy(true);
        try
        {
            var result = await _client.ImportDictionaryAsync(path, overwrite: false, default);
            if (result.Status == SettingsStatus.Exists)
            {
                var name = System.IO.Path.GetFileNameWithoutExtension(fileName);
                var replace = await ConfirmAsync(
                    $"已存在同名词库「{name}」。要用这个文件覆盖它吗？",
                    "覆盖", "跳过");
                if (!replace)
                {
                    Report($"已跳过 {fileName}：保留了原有的同名词库。");
                    return;
                }

                result = await _client.ImportDictionaryAsync(path, overwrite: true, default);
            }

            if (result.Status == SettingsStatus.Ok && result.Value is { } imported)
            {
                Apply(imported.List);
                Report($"已导入 {imported.Entry.Name}（{imported.Entry.SizeLabel}）。");
            }
            else
            {
                Report($"{fileName}：{Describe(result.Status, result.Error)}");
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task RunAsync(Func<SettingsClient, Task<SettingsResult<DictionaryList>>> operation)
    {
        SetBusy(true);
        try
        {
            var result = await operation(_client);
            if (result.Status == SettingsStatus.Ok && result.Value is { } list)
            {
                Apply(list);
                Report("词库列表已更新。");
            }
            else
            {
                Report(Describe(result.Status, result.Error));
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task SetEnabledAsync(DictionaryEntry entry, bool enabled)
    {
        SetBusy(true);
        try
        {
            var result = await _client.SetDictionaryEnabledAsync(entry.Name, enabled, default);
            if (result.Status == SettingsStatus.Ok && result.Value is { } list)
            {
                Apply(list);
                Report($"{entry.Name}：{(enabled ? "已启用" : "已停用")}。");
            }
            else
            {
                Report($"{entry.Name}：{Describe(result.Status, result.Error)}");
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task DeleteAsync(DictionaryEntry entry)
    {
        var confirmed = await ConfirmAsync(
            $"删除词库「{entry.Name}」？只会删除这个文件和它的启用标记，不会触碰学习记录或系统词库。",
            "删除", "取消");
        if (!confirmed)
        {
            return;
        }

        SetBusy(true);
        try
        {
            var result = await _client.DeleteDictionaryAsync(entry.Name, default);
            if (result.Status == SettingsStatus.Ok && result.Value is { } list)
            {
                Apply(list);
                Report($"已删除 {entry.Name}。");
            }
            else
            {
                Report($"{entry.Name}：{Describe(result.Status, result.Error)}");
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void Apply(DictionaryList list)
    {
        _rows.Children.Clear();
        if (list.Entries.Count == 0)
        {
            _rows.Children.Add(new TextBlock
            {
                Text = "还没有用户词库。",
                Opacity = 0.7,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            });
            return;
        }

        foreach (var entry in list.Entries)
        {
            _rows.Children.Add(BuildRow(entry));
        }
    }

    private FrameworkElement BuildRow(DictionaryEntry entry)
    {
        var grid = new Grid { ColumnSpacing = 16 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var left = new StackPanel { Spacing = 2 };
        left.Children.Add(new TextBlock
        {
            Text = entry.Name,
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
        });
        left.Children.Add(new TextBlock
        {
            Text = $"{entry.SourceLabel} · {entry.StateLabel} · {entry.SizeLabel}",
            Opacity = 0.7,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        });
        grid.Children.Add(left);

        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var toggle = new Button { Content = entry.Enabled ? "停用" : "启用" };
        toggle.Click += async (_, _) => await SetEnabledAsync(entry, !entry.Enabled);
        actions.Children.Add(toggle);

        if (entry.CanDelete)
        {
            var delete = new Button { Content = "删除" };
            delete.Click += async (_, _) => await DeleteAsync(entry);
            actions.Children.Add(delete);
        }

        Grid.SetColumn(actions, 1);
        grid.Children.Add(actions);
        return grid;
    }

    private void Report(string text) => _status.Text = text;

    private void SetBusy(bool busy)
    {
        _busy = busy;
        if (_importButton is not null)
        {
            _importButton.IsEnabled = !busy;
        }

        if (_reloadButton is not null)
        {
            _reloadButton.IsEnabled = !busy;
        }
    }

    private async Task<bool> ConfirmAsync(string message, string primary, string close)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = primary,
            Content = message,
            PrimaryButtonText = primary,
            CloseButtonText = close,
            DefaultButton = ContentDialogButton.Close,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    private static string Describe(SettingsStatus status, string error) => status switch
    {
        SettingsStatus.Busy => "正在组合中，词库没有改动。结束或取消当前输入后再试。",
        SettingsStatus.Exists => string.IsNullOrEmpty(error) ? "同名词库已存在。" : error,
        SettingsStatus.Unsupported or SettingsStatus.Invalid or SettingsStatus.Error
            when !string.IsNullOrEmpty(error) => error,
        SettingsStatus.Indeterminate => string.IsNullOrEmpty(error) ? "结果不确定，请重新加载确认。" : error,
        _ => Fusion.Settings.Workspace.SettingsWorkspace.Describe(status, error),
    };
}
