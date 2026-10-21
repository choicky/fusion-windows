using System;
using System.Threading.Tasks;
using Fusion.Settings.App.Services;
using Fusion.Settings.Workspace;
using Microsoft.UI.Xaml;

namespace Fusion.Settings.App;

/// <summary>
/// Application entry point. Resolves the endpoint exactly as the Qt client did, so an
/// isolated <c>--pipe-name</c> instance is honoured and a malformed one is refused rather
/// than silently pointed at the production Host.
/// </summary>
public partial class App : Application
{
    private Window? _window;

    public App()
    {
        Trace.Reset();
        Trace.Write("app ctor");
        InitializeComponent();
        UnhandledException += OnUnhandledException;
    }

    /// <summary>The workspace the window and its pages share.</summary>
    public SettingsWorkspace? Workspace { get; private set; }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Trace.Write("onlaunched");
        var endpoint = ResolveEndpoint();
        Trace.Write("endpoint=" + (endpoint ?? "(null)"));
        if (endpoint is null)
        {
            // Nothing was created, so there is no window to attach a dialog to. Say it in
            // the only way available and exit rather than opening a window that cannot work.
            Environment.Exit(2);
            return;
        }

        Workspace = new SettingsWorkspace(endpoint);
        Trace.Write("workspace created");
        _window = new MainWindow(Workspace);
        Trace.Write("mainwindow created");
        _window.Activate();
        Trace.Write("mainwindow activated");
    }

    /// <summary>
    /// <c>--pipe-name &lt;name&gt;</c> overrides the production endpoint, for tests and for a
    /// second instance. The same normalisation the Host uses applies: a value that is not
    /// a single pipe name is rejected, never replaced by the default.
    /// </summary>
    private static string? ResolveEndpoint()
    {
        var arguments = Environment.GetCommandLineArgs();
        for (var i = 1; i < arguments.Length; i++)
        {
            if (!string.Equals(arguments[i], "--pipe-name", StringComparison.Ordinal))
            {
                continue;
            }

            if (i + 1 >= arguments.Length)
            {
                return null;
            }

            return Client.PipeEndpoint.Normalize(arguments[i + 1]);
        }

        return Client.PipeEndpoint.Production();
    }

    private void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        Trace.Write("UNHANDLED: " + e.Message);
        // Keep the app alive long enough to report, instead of vanishing silently.
        System.Diagnostics.Debug.WriteLine("unhandled: " + e.Message);
    }
}
