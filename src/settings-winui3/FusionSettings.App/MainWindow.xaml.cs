using System;
using System.ComponentModel;
using System.Threading.Tasks;
using Fusion.Settings.App.Services;
using Fusion.Settings.App.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App;

/// <summary>
/// The settings window: a stable navigation area, a readable content column, and one
/// save bar that always shows what is pending and what the last save did.
/// </summary>
public sealed partial class MainWindow : Window
{
    private readonly SettingsWorkspace _workspace;
    private bool _initialised;
    private string? _currentTag;

    public MainWindow(SettingsWorkspace workspace)
    {
        _workspace = workspace;
        InitializeComponent();

        Title = "Fusion 设置";
        StatusLine.Text = "正在连接 FusionHost…";

        Trace.Write("mainwindow ctor");
        _workspace.PropertyChanged += OnWorkspaceChanged;
        AppWindow.Closing += OnClosing;
        Activated += OnFirstActivated;
    }

    private async void OnFirstActivated(object sender, WindowActivatedEventArgs args)
    {
        if (_initialised)
        {
            return;
        }

        _initialised = true;
        Trace.Write("activated");

        // Selecting the first item shows the page, so the window is never empty while the
        // Host is contacted; the load then fills it in.
        Navigation.SelectedItem = Navigation.MenuItems[0];

        Trace.Write("load begin");
        await LoadAsync().ConfigureAwait(true);
        Trace.Write("load end");
        RefreshCurrentPage();
        Trace.Write("page refreshed");
    }

    private async Task LoadAsync()
    {
        await _workspace.InitializeAsync().ConfigureAwait(true);
        UpdateStatus();
        Navigate("methods");
    }

    private void OnNavigationSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item && item.Tag is string tag)
        {
            Navigate(tag);
        }
    }

    private void Navigate(string tag)
    {
        if (PageHost is null)
        {
            return;
        }

        if (_currentTag == tag && PageHost.Content is SettingsPageBase)
        {
            return;
        }

        _currentTag = tag;

        // Navigation never discards edits: the draft lives in the workspace, not in a page.
        // Deferred to the dispatcher so the page is built after the current layout pass.
        DispatcherQueue.TryEnqueue(() =>
        {
            if (_currentTag != tag)
            {
                return;
            }

            Trace.Write("navigate " + tag);
            var page = tag switch
            {
                "keys" => new KeysPage(),
                "candidates" => new CandidatesPage(),
                "about" => new AboutPage(),
                _ => (SettingsPageBase)new InputMethodsPage(),
            };

            page.Initialize(_workspace);
            PageHost.Content = page;
            Trace.Write("navigated " + tag);
        });
    }

    private void OnWorkspaceChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(SettingsWorkspace.StatusText):
            case nameof(SettingsWorkspace.IsBusy):
            case nameof(SettingsWorkspace.HasChanges):
                UpdateStatus();
                break;
            case nameof(SettingsWorkspace.SaveSummary):
                UpdateStatus();
                break;
        }
    }

    private void UpdateStatus()
    {
        StatusLine.Text = _workspace.IsBusy ? "正在与 Host 通信…" : _workspace.StatusText;

        var summary = _workspace.SaveSummary;
        SaveLine.Text = summary;
        SaveLine.Visibility = string.IsNullOrEmpty(summary) ? Visibility.Collapsed : Visibility.Visible;

        var canSave = _workspace.HasChanges && !_workspace.IsBusy;
        SaveButton.IsEnabled = canSave;
        DiscardButton.IsEnabled = canSave;

        // A pending edit is always visible in the save bar, so the window never looks saved
        // while something is still only in the draft.
        if (_workspace.HasChanges && !_workspace.IsBusy)
        {
            StatusLine.Text = _workspace.StatusText + " 有未保存的修改。";
        }
    }

    private async void OnSaveClick(object sender, RoutedEventArgs e)
    {
        await _workspace.SaveAsync().ConfigureAwait(true);
        UpdateStatus();
        RefreshCurrentPage();
    }

    private void OnDiscardClick(object sender, RoutedEventArgs e)
    {
        _workspace.Discard();
        UpdateStatus();
        RefreshCurrentPage();
    }

    private void RefreshCurrentPage()
    {
        if (PageHost.Content is SettingsPageBase page)
        {
            page.Refresh();
        }
    }

    /// <summary>
    /// Closing with unsaved edits asks first. The draft survives navigation, so the only
    /// way to lose work silently would be to close the window without asking.
    /// </summary>
    private async void OnClosing(Microsoft.UI.Windowing.AppWindow sender,
                                 Microsoft.UI.Windowing.AppWindowClosingEventArgs args)
    {
        if (!_workspace.HasChanges)
        {
            return;
        }

        args.Cancel = true;

        var dialog = new ContentDialog
        {
            XamlRoot = PageHost.XamlRoot,
            Title = "还有未保存的修改",
            Content = "关闭窗口会丢弃这些修改。要保存后再关闭吗？",
            PrimaryButtonText = "保存并关闭",
            SecondaryButtonText = "放弃并关闭",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
        };

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary)
        {
            await _workspace.SaveAsync().ConfigureAwait(true);
            if (_workspace.HasChanges)
            {
                // The save did not take; keep the window open rather than lose the edits.
                return;
            }
        }
        else if (result == ContentDialogResult.None)
        {
            return;
        }

        _workspace.Discard();
        Close();
    }
}
