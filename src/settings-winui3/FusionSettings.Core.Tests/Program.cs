using System;
using System.Threading.Tasks;

namespace Fusion.Settings.Tests;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        Console.WriteLine("Fusion settings client core — focused checks");
        Console.WriteLine();

        WireTests.Run();
        ModelTests.Run();
        KeyTests.Run();
        ImListAndEndpointTests.Run();

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
}
