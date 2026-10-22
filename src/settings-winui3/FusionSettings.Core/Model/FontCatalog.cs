using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace Fusion.Settings.Model;

/// <summary>
/// The font families this Windows session has, from GDI's own font enumeration.
/// </summary>
/// <remarks>
/// <para>
/// GDI is the right source here rather than a convenience: the candidate window paints with GDI
/// (<c>CreateFontW</c> from the configured family name, then <c>DrawTextW</c>), so a name in this
/// list is a name that window can actually create and draw. Enumerating the registry's font list
/// instead would report registrations rather than the collection GDI loads, and a name read from
/// there can be one no face resolves to.
/// </para>
/// <para>
/// Only names are taken: nothing is copied, packaged, or cached on disk. The list is a read of
/// the session's current state, so a font installed after the window opened appears on the next
/// read (<see cref="Invalidate"/>).
/// </para>
/// </remarks>
public static class FontCatalog
{
    private static readonly object Gate = new();
    private static IReadOnlyList<string>? _cached;

    /// <summary>
    /// Every installed family, once each, sorted the way a picker shows it: case-insensitively,
    /// then ordinally so the order is stable between reads.
    /// </summary>
    public static IReadOnlyList<string> InstalledFamilies()
    {
        lock (Gate)
        {
            return _cached ??= Enumerate();
        }
    }

    /// <summary>Forgets a cached read, so a font installed while the window is open shows up.</summary>
    public static void Invalidate()
    {
        lock (Gate)
        {
            _cached = null;
        }
    }

    /// <summary>
    /// The installed family a stored value names, or null when it names none. The comparison is
    /// ordinal-ignore-case: a family can be written with different casing by different tools and
    /// Windows resolves it the same way.
    /// </summary>
    public static string? Find(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var trimmed = value.Trim();
        return InstalledFamilies().FirstOrDefault(
            name => string.Equals(name, trimmed, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// The families whose name contains <paramref name="query"/>, in catalogue order. An empty
    /// query returns everything, which is what the unfiltered list shows.
    /// </summary>
    public static IReadOnlyList<string> Search(string? query)
    {
        var families = InstalledFamilies();
        if (string.IsNullOrWhiteSpace(query))
        {
            return families;
        }

        var needle = query.Trim();
        return families
            .Where(name => name.Contains(needle, StringComparison.OrdinalIgnoreCase))
            .ToList();
    }

    private static List<string> Enumerate()
    {
        var names = new List<string>();
        // Only the name is read from each face, so one entry per face is one entry per weight
        // and style; de-duplicating by name is what turns that into one row per family.
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        var hdc = GetDC(IntPtr.Zero);
        if (hdc == IntPtr.Zero)
        {
            return names;
        }

        try
        {
            var query = new LOGFONT { lfCharSet = DefaultCharset };
            EnumFontEx callback = (ref LOGFONT face, IntPtr textMetric, uint fontType, IntPtr lParam) =>
            {
                var name = face.lfFaceName;
                // A leading '@' marks the vertical-writing alias of a family GDI also lists
                // horizontally (for example "@微软雅黑"). The candidate window draws
                // horizontally, so the alias is the same family under a name the user would not
                // recognise, and it is dropped rather than offered as a second choice.
                if (!string.IsNullOrWhiteSpace(name) && name[0] != '@' && seen.Add(name))
                {
                    names.Add(name);
                }

                return 1; // continue
            };

            // The default charset asks for every face of every family, whatever script it
            // covers; a specific charset would return one face per family for that script only.
            _ = EnumFontFamiliesEx(hdc, ref query, callback, IntPtr.Zero, 0);
            GC.KeepAlive(callback);
        }
        finally
        {
            _ = ReleaseDC(IntPtr.Zero, hdc);
        }

        names.Sort(static (left, right) =>
        {
            var byCase = string.Compare(left, right, StringComparison.OrdinalIgnoreCase);
            return byCase != 0 ? byCase : string.CompareOrdinal(left, right);
        });
        return names;
    }

    /// <summary>DEFAULT_CHARSET: every face GDI has, not just the ones for one code page.</summary>
    private const byte DefaultCharset = 1;

    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode)]
    private delegate int EnumFontEx(ref LOGFONT logFont, IntPtr textMetric, uint fontType,
        IntPtr lParam);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumFontFamiliesExW")]
    private static extern int EnumFontFamiliesEx(IntPtr hdc, ref LOGFONT logFont,
        EnumFontEx callback, IntPtr lParam, uint flags);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr window, IntPtr hdc);

    /// <summary>LOGFONTW, the head of the face struct GDI's enumeration hands back.</summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct LOGFONT
    {
        public int lfHeight;
        public int lfWidth;
        public int lfEscapement;
        public int lfOrientation;
        public int lfWeight;
        public byte lfItalic;
        public byte lfUnderline;
        public byte lfStrikeOut;
        public byte lfCharSet;
        public byte lfOutPrecision;
        public byte lfClipPrecision;
        public byte lfQuality;
        public byte lfPitchAndFamily;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string lfFaceName;
    }
}
