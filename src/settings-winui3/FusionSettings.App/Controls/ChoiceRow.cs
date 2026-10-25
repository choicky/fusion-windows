using System;
using System.ComponentModel;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// One row of a list that shows a current value: the text, the value it stands for, and whether
/// it is the one in effect.
/// </summary>
/// <remarks>
/// <para>
/// The mark is a row style, never part of the text: <see cref="Name"/> is what the control shows
/// and what a screen reader announces, and <see cref="Value"/> is what the Host round-trips. A
/// list that has no current value simply leaves every row unmarked.
/// </para>
/// <para>
/// The mark follows the value the control holds, so a draft value that has not been saved yet is
/// marked exactly like a stored one. <see cref="IsCurrent"/> raises change notifications, which is
/// what lets a dropdown move the mark from the old row to the new one without rebuilding its
/// items — rebuilding them would reset the selection and turn a mark into an edit.
/// </para>
/// </remarks>
public sealed class ChoiceRow : INotifyPropertyChanged
{
    private bool _isCurrent;

    public ChoiceRow(string name, string? value = null, bool isCurrent = false)
    {
        Name = name ?? string.Empty;
        Value = value;
        _isCurrent = isCurrent;
    }

    /// <summary>The text the row shows.</summary>
    public string Name { get; }

    /// <summary>The stored value the row stands for, when the row is a list of values.</summary>
    public string? Value { get; }

    public bool IsCurrent
    {
        get => _isCurrent;
        set
        {
            if (_isCurrent == value)
            {
                return;
            }

            _isCurrent = value;
            Raise(nameof(IsCurrent));
            Raise(nameof(Background));
            Raise(nameof(MarkerVisibility));
        }
    }

    /// <summary>The fill the row is drawn on: the accent tint when it is the value in effect.</summary>
    public Brush Background => _isCurrent ? ChoiceMark.CurrentFill : ChoiceMark.Transparent;

    /// <summary>Whether the accent bar at the left edge is drawn.</summary>
    public Visibility MarkerVisibility => _isCurrent ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>The accent bar itself, so the template needs no theme lookup of its own.</summary>
    public Brush MarkerBrush => ChoiceMark.AccentBar;

    public event PropertyChangedEventHandler? PropertyChanged;

    public override string ToString() => Name;

    private void Raise(string property) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
}

/// <summary>
/// The one visual a selection list uses to show which row is the value in effect: an accent bar at
/// the left edge and a translucent accent fill, both of them only on that row.
/// </summary>
/// <remarks>
/// <para>
/// The fill is the theme's own accent colour at a quarter of its strength. A translucent accent
/// reads on the light and the dark surface alike, and it is clearly heavier than the fills hover and
/// keyboard navigation draw, so the three states stay distinguishable: the mark says which value is
/// in effect, the list's own fills say where the pointer or the keyboard is.
/// </para>
/// <para>
/// The bar is what makes the row identifiable at a glance without relying on a background contrast
/// that a high-contrast theme may flatten; it is a shape, not a colour difference, so it survives
/// the themes that recolour every fill.
/// </para>
/// </remarks>
internal static class ChoiceMark
{
    public static readonly SolidColorBrush Transparent = new(Colors.Transparent);

    private static SolidColorBrush? currentFill_;
    private static SolidColorBrush? accentBar_;
    private static DataTemplate? template_;

    public static Brush CurrentFill
    {
        get
        {
            if (currentFill_ is null)
            {
                var accent = (Windows.UI.Color)Application.Current.Resources["SystemAccentColor"];
                currentFill_ = new SolidColorBrush(
                    Windows.UI.Color.FromArgb(0x40, accent.R, accent.G, accent.B));
            }

            return currentFill_;
        }
    }

    /// <summary>The bar: the accent colour at full strength, so it reads as a mark and not a wash.</summary>
    public static Brush AccentBar
    {
        get
        {
            if (accentBar_ is null)
            {
                var accent = (Windows.UI.Color)Application.Current.Resources["SystemAccentColor"];
                accentBar_ = new SolidColorBrush(accent);
            }

            return accentBar_;
        }
    }

    /// <summary>
    /// A list this class cannot style row by row — a WinUI dropdown draws the selected row with its
    /// own template — gets the same fill through the resources that template reads, so both kinds
    /// of list end up looking the same without either one being restyled.
    /// </summary>
    /// <remarks>
    /// The dropdown's own left indicator and its hover/navigation fills are left exactly as they
    /// are: the mark is the fill, and the control already draws the bar.
    /// </remarks>
    public static void ApplyTo(ComboBox combo)
    {
        foreach (var key in new[]
                 {
                     "ComboBoxItemBackgroundSelected",
                     "ComboBoxItemBackgroundSelectedPointerOver",
                     "ComboBoxItemBackgroundSelectedPressed",
                 })
        {
            combo.Resources[key] = CurrentFill;
        }
    }

    /// <summary>
    /// The row layout for a list whose rows this class draws. Every row reserves the bar's width, so
    /// the text of a marked row and of an unmarked one line up, and the row's own fill is the only
    /// thing that changes with the mark.
    /// </summary>
    public static DataTemplate Template =>
        template_ ??= (DataTemplate)XamlReader.Load(
            "<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>" +
            "<Grid Background='{Binding Background}' CornerRadius='4' Padding='8,4' ColumnSpacing='8'>" +
            "<Grid.ColumnDefinitions>" +
            "<ColumnDefinition Width='Auto'/><ColumnDefinition Width='*'/>" +
            "</Grid.ColumnDefinitions>" +
            "<Border Width='3' Height='16' CornerRadius='2' Background='{Binding MarkerBrush}'" +
            " Visibility='{Binding MarkerVisibility}' VerticalAlignment='Center'/>" +
            "<TextBlock Grid.Column='1' Text='{Binding Name}' TextTrimming='CharacterEllipsis'" +
            " VerticalAlignment='Center'/>" +
            "</Grid></DataTemplate>");

    private static DataTemplate? textTemplate_;

    /// <summary>
    /// The row layout for a list that draws the selection itself: the text, and nothing else, so the
    /// control's own mark is not doubled by one of ours.
    /// </summary>
    public static DataTemplate TextTemplate =>
        textTemplate_ ??= (DataTemplate)XamlReader.Load(
            "<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>" +
            "<TextBlock Text='{Binding Name}' TextTrimming='CharacterEllipsis'" +
            " VerticalAlignment='Center'/>" +
            "</DataTemplate>");
}
