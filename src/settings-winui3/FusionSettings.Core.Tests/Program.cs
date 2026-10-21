using System;
using System.Threading;
using System.Threading.Tasks;
using Fusion.Settings.Client;

namespace Fusion.Settings.Tests;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        Console.WriteLine("Fusion settings client core — focused checks");
        Console.WriteLine();

        // A post-restart check is a different run on purpose: the Host has been stopped and
        // started again against the same profile, and all this run does is read what the
        // previous one wrote. Nothing is edited, so nothing is overwritten.
        var expectation = Environment.GetEnvironmentVariable("FUSION_SETTINGS_GLOBAL_EXPECT");
        if (!string.IsNullOrWhiteSpace(expectation))
        {
            return await VerifyPersistedAsync(expectation).ConfigureAwait(false);
        }

        WireTests.Run();
        ModelTests.Run();
        KeyTests.Run();
        ImListAndEndpointTests.Run();

        // The controlled-peer checks use a real pipe in this process, so they need no Host.
        await ClientTests.RunAsync().ConfigureAwait(false);

        // An uncertain method-list write and a reconnect, driven through the workspace against
        // a scripted peer: both are sequences of replies, not single-response cases.
        await RecoveryTests.RunAsync().ConfigureAwait(false);

        // The workspace path needs a real Host; it skips without one.
        await WorkspaceTests.RunAsync().ConfigureAwait(false);

        // The global save needs a real Host with a writable profile; it skips without one.
        await GlobalSaveSectionAsync().ConfigureAwait(false);

        // The real-Host checks run only when an isolated endpoint is supplied; they never
        // fall back to the installed product's pipe.
        var includeIntegration = args.Length == 0 || Array.IndexOf(args, "--no-integration") < 0;
        if (includeIntegration)
        {
            await IntegrationTests.RunAsync().ConfigureAwait(false);
        }
        else
        {
            Console.WriteLine("-- integration: SKIPPED (--no-integration)");
        }

        return Harness.Report();
    }

    private static async Task GlobalSaveSectionAsync()
    {
        var profile = Environment.GetEnvironmentVariable("FUSION_SETTINGS_GLOBAL_PROFILE");
        if (string.IsNullOrWhiteSpace(profile))
        {
            Harness.Section("global: SKIPPED (FUSION_SETTINGS_GLOBAL_PROFILE is not set)");
            return;
        }

        if (!TryIsolatedEndpoint(out var endpoint))
        {
            return;
        }

        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(120));
        await GlobalSaveTests.RunAsync(endpoint, profile, cancellation.Token).ConfigureAwait(false);
    }

    private static async Task<int> VerifyPersistedAsync(string expectation)
    {
        if (!TryIsolatedEndpoint(out var endpoint))
        {
            return Harness.Report();
        }

        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        await GlobalSaveTests.VerifyPersistedAsync(endpoint, expectation, cancellation.Token)
            .ConfigureAwait(false);
        return Harness.Report();
    }

    private static bool TryIsolatedEndpoint(out string endpoint)
    {
        endpoint = string.Empty;
        var configured = Environment.GetEnvironmentVariable("FUSION_SETTINGS_TEST_PIPE");
        if (string.IsNullOrWhiteSpace(configured))
        {
            Harness.Section("global: SKIPPED (FUSION_SETTINGS_TEST_PIPE is not set)");
            return false;
        }

        var normalized = PipeEndpoint.Normalize(configured);
        if (normalized is null || PipeEndpoint.IsProduction(normalized))
        {
            throw new InvalidOperationException(
                "FUSION_SETTINGS_TEST_PIPE must name an isolated endpoint, not the production one");
        }

        endpoint = normalized;
        return true;
    }
}
