using System;
using System.Collections.Generic;
using System.Text;
using Fusion.Settings.Model;
using Windows.System;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// Translates between what the Host stores (X11 keysym names) and what a person reads.
/// </summary>
/// <remarks>
/// The names matter: Fcitx stores <c>bracketleft</c>, <c>bracketright</c>, <c>semicolon</c>
/// and <c>Next</c>, while the source constants read <c>[</c>, <c>]</c>, <c>;</c> and
/// <c>Page_Down</c>. Showing the stored name unlocalised would look like a different key
/// from the one the user pressed, so every tag shows a readable label and keeps the stored
/// name in its tooltip.
/// </remarks>
public static class KeyNames
{
    private static readonly Dictionary<string, string> Labels = new(StringComparer.Ordinal)
    {
        ["comma"] = "，",
        ["period"] = "。",
        ["semicolon"] = "；",
        ["apostrophe"] = "’",
        ["slash"] = "/",
        ["backslash"] = "＼",
        ["bracketleft"] = "［",
        ["bracketright"] = "］",
        ["minus"] = "-",
        ["equal"] = "=",
        ["grave"] = "`",
        ["space"] = "空格",
        ["Return"] = "回车",
        ["KP_Enter"] = "小键盘回车",
        ["Tab"] = "Tab",
        ["BackSpace"] = "退格",
        ["Escape"] = "Esc",
        ["Delete"] = "Delete",
        ["Up"] = "↑",
        ["Down"] = "↓",
        ["Left"] = "←",
        ["Right"] = "→",
        ["Home"] = "Home",
        ["End"] = "End",
        ["Page_Up"] = "Page Up",
        ["Next"] = "Page Down",
        ["Insert"] = "Insert",
        ["Shift_L"] = "左 Shift",
        ["Shift_R"] = "右 Shift",
        ["Control_L"] = "左 Ctrl",
        ["Control_R"] = "右 Ctrl",
        ["Alt_L"] = "左 Alt",
        ["Alt_R"] = "右 Alt",
        ["Super_L"] = "左 Win",
        ["Super_R"] = "右 Win",
    };

    /// <summary>A readable label for one chord, keeping the stored name available.</summary>
    public static string Label(KeyChord chord)
    {
        if (chord.IsOpaque)
        {
            return chord.OpaqueText!;
        }

        if (chord.IsEmpty)
        {
            return "（空）";
        }

        var text = new StringBuilder();
        foreach (var modifier in chord.Modifiers)
        {
            text.Append(modifier switch
            {
                "Control" => "Ctrl",
                "Super" => "Win",
                _ => modifier,
            });
            text.Append('+');
        }

        text.Append(Labels.TryGetValue(chord.KeyName, out var label) ? label : chord.KeyName);
        return text.ToString();
    }

    /// <summary>The stored form, for a tooltip: the label alone would hide the real name.</summary>
    public static string Stored(KeyChord chord) =>
        chord.IsOpaque ? $"无法识别的绑定，按原样保留：{chord.OpaqueText}" : chord.ToString();

    /// <summary>
    /// Builds a chord from a Windows key press. Modifiers are taken from the keyboard state
    /// so <c>Ctrl+Return</c> records as one chord, not as two tags.
    /// </summary>
    public static KeyChord? FromKey(VirtualKey key, bool control, bool alt, bool shift, bool windows)
    {
        var name = KeysymName(key);
        if (name is null)
        {
            return null;
        }

        var text = new StringBuilder();
        if (control)
        {
            text.Append("Control+");
        }

        if (alt)
        {
            text.Append("Alt+");
        }

        if (shift)
        {
            text.Append("Shift+");
        }

        if (windows)
        {
            text.Append("Super+");
        }

        text.Append(name);
        return KeyChord.Parse(text.ToString());
    }

    /// <summary>The keysym name for a Windows virtual key, or null when it is not a key we name.</summary>
    private static string? KeysymName(VirtualKey key) => key switch
    {
        >= VirtualKey.A and <= VirtualKey.Z => ((char)('a' + (key - VirtualKey.A))).ToString(),
        >= VirtualKey.Number0 and <= VirtualKey.Number9 => ((char)('0' + (key - VirtualKey.Number0))).ToString(),
        >= VirtualKey.NumberPad0 and <= VirtualKey.NumberPad9 =>
            "KP_" + (char)('0' + (key - VirtualKey.NumberPad0)),
        >= VirtualKey.F1 and <= VirtualKey.F12 => "F" + (key - VirtualKey.F1 + 1),
        VirtualKey.Space => "space",
        VirtualKey.Enter => "Return",
        VirtualKey.Tab => "Tab",
        VirtualKey.Escape => "Escape",
        VirtualKey.Back => "BackSpace",
        VirtualKey.Delete => "Delete",
        VirtualKey.Insert => "Insert",
        VirtualKey.Up => "Up",
        VirtualKey.Down => "Down",
        VirtualKey.Left => "Left",
        VirtualKey.Right => "Right",
        VirtualKey.Home => "Home",
        VirtualKey.End => "End",
        VirtualKey.PageUp => "Page_Up",
        // The canonical keysym name for Page Down is Next; Fcitx stores and returns "Next".
        VirtualKey.PageDown => "Next",
        VirtualKey.Shift => null,
        VirtualKey.Control => null,
        VirtualKey.Menu => null,
        (VirtualKey)0x5B => null,
        (VirtualKey)0x5C => null,
        (VirtualKey)0xBC => "comma",
        (VirtualKey)0xBE => "period",
        (VirtualKey)0xBA => "semicolon",
        (VirtualKey)0xDE => "apostrophe",
        (VirtualKey)0xBF => "slash",
        (VirtualKey)0xDC => "backslash",
        (VirtualKey)0xDB => "bracketleft",
        (VirtualKey)0xDD => "bracketright",
        (VirtualKey)0xBD => "minus",
        (VirtualKey)0xBB => "equal",
        (VirtualKey)0xC0 => "grave",
        VirtualKey.Add => "KP_Add",
        VirtualKey.Subtract => "KP_Subtract",
        VirtualKey.Multiply => "KP_Multiply",
        VirtualKey.Divide => "KP_Divide",
        VirtualKey.Decimal => "KP_Decimal",
        _ => null,
    };
}
