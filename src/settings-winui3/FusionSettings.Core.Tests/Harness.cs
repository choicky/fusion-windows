using System;
using System.Collections.Generic;

namespace Fusion.Settings.Tests;

/// <summary>A minimal assertion runner: one failure list, a non-zero exit on any failure.</summary>
internal static class Harness
{
    private static readonly List<string> Failures = new();
    private static string _section = string.Empty;
    private static int _checks;

    public static void Section(string name)
    {
        _section = name;
        Console.WriteLine($"-- {name}");
    }

    public static void Check(bool condition, string what)
    {
        _checks++;
        if (condition)
        {
            return;
        }

        var message = $"{_section}: {what}";
        Failures.Add(message);
        Console.WriteLine($"   FAIL {what}");
    }

    public static void CheckEqual<T>(T expected, T actual, string what)
    {
        var equal = EqualityComparer<T>.Default.Equals(expected, actual);
        Check(equal, equal ? what : $"{what} (expected {Format(expected)}, got {Format(actual)})");
    }

    public static void Throws<TException>(Action action, string what)
        where TException : Exception
    {
        try
        {
            action();
            Check(false, $"{what} (no exception)");
        }
        catch (TException)
        {
            Check(true, what);
        }
        catch (Exception exception)
        {
            Check(false, $"{what} (threw {exception.GetType().Name})");
        }
    }

    private static string Format<T>(T value) => value is null ? "null" : value.ToString() ?? "null";

    public static int Report()
    {
        Console.WriteLine();
        if (Failures.Count == 0)
        {
            Console.WriteLine($"PASS {_checks} checks");
            return 0;
        }

        Console.WriteLine($"FAIL {Failures.Count} of {_checks} checks");
        foreach (var failure in Failures)
        {
            Console.WriteLine($"  - {failure}");
        }

        return 1;
    }
}
