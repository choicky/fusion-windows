using System;
using System.ComponentModel;
using System.Threading.Tasks;
using Fusion.Settings.App.Services;
using Fusion.Settings.App.Views;
using Fusion.Settings.Workspace;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace Fusion.Settings.App;

/// <summary>
/// The settings window: a stable navigation area, a readable content column, and one
/// save bar that always shows what is pending, what the last save did, and how to recover.
/// </summary>
public sealed partial class MainWindow : Window
{
    private readonly SettingsWorkspace _workspace;
    private bool _initialised;
    private string? _currentTag;
    private bool _closeDialogOpen;

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
        await _workspace.InitializeAsync().ConfigureAwait(true);
        Trace.Write("load end");
        RefreshCurrentPage();
        UpdateStatus();
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
            UpdateStatus();
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
            case nameof(SettingsWorkspace.SaveSummary):
            case nameof(SettingsWorkspace.IsLoaded):
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

        var idle = !_workspace.IsBusy;
        var canSave = _workspace.HasChanges && idle;
        SaveButton.IsEnabled = canSave;
        DiscardButton.IsEnabled = canSave;

        // Editing is disabled while an operation owns the Host, so nothing can change under a
        // request that has already been captured.
        PageHost.IsEnabled = _workspace.CanEdit;

        RetryButton.Visibility = _workspace.NeedsRetry ? Visibility.Visible : Visibility.Collapsed;
        RetryButton.IsEnabled = idle;

        // A reload is only useful once something has loaded; before that the retry is the action.
        ReloadButton.Visibility = _workspace.IsLoaded ? Visibility.Visible : Visibility.Collapsed;
        ReloadButton.IsEnabled = idle;

        if (_workspace.HasChanges && idle)
        {
            StatusLine.Text = _workspace.StatusText + " 有未保存的修改。";
        }
    }

    private async void OnSaveClick(object sender, RoutedEventArgs e)
    {
        // A focused TextBox or NumberBox commits on lost focus; move focus first so Save sees
        // what the user typed rather than the previous value.
        CommitPendingInput();

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

    private async void OnRetryClick(object sender, RoutedEventArgs e)
    {
        // A retry after a failed start: nothing was loaded, so there is nothing to preserve.
        await _workspace.InitializeAsync().ConfigureAwait(true);
        UpdateStatus();
        RefreshCurrentPage();
    }

    private async void OnReloadClick(object sender, RoutedEventArgs e)
    {
        if (_workspace.HasChanges)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = PageHost.XamlRoot,
                Title = "重新加载会丢弃未保存的修改",
                Content = "要继续吗？",
                PrimaryButtonText = "丢弃并重新加载",
                CloseButtonText = "取消",
                DefaultButton = ContentDialogButton.Close,
            };

            if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            {
                return;
            }

            _workspace.Discard();
        }

        await _workspace.ReloadAsync(preserveEdits: false).ConfigureAwait(true);
        UpdateStatus();
        RefreshCurrentPage();
    }

    /// <summary>
    /// Moves focus off whatever is being edited so a lost-focus commit runs before the draft is
    /// read. Harmless when nothing has focus.
    /// </summary>
    private void CommitPendingInput()
    {
        if (PageHost.XamlRoot is null || PageHost.Content is null)
        {
            return;
        }

        if (FocusManager.GetFocusedElement(PageHost.XamlRoot) is null)
        {
            return;
        }

        try
        {
            // The desktop overload needs an explicit search root; the bare overload throws in a
            // WinUI Desktop app and takes the window down with it.
            FocusManager.TryMoveFocus(
                FocusNavigationDirection.Next,
                new FindNextElementOptions { SearchRoot = PageHost });
        }
        catch (Exception exception)
        {
            // Moving focus is a convenience: a failure must never block a save.
            Trace.Write("commit focus failed: " + exception.Message);
        }
    }

    private void RefreshCurrentPage()
    {
        if (PageHost.Content is SettingsPageBase page)
        {
            page.Refresh();
        }
    }

    /// <summary>
    /// Closing with unsaved edits asks first, once. The draft survives navigation, so closing
    /// the window is the only way to lose work without being asked.
    /// </summary>
    private async void OnClosing(Microsoft.UI.Windowing.AppWindow sender,
                                 Microsoft.UI.Windowing.AppWindowClosingEventArgs args)
    {
        if (!_workspace.HasChanges)
        {
            return;
        }

        args.Cancel = true;

        // A second close request while the dialog is open must not stack another dialog.
        if (_closeDialogOpen)
        {
            return;
        }

        _closeDialogOpen = true;
        try
        {
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
                CommitPendingInput();
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
        finally
        {
            _closeDialogOpen = false;
        }
    }
}
