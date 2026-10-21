using System;
using Windows.Foundation;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Fusion.Settings.App.Controls;

/// <summary>
/// Lays its children out left to right and wraps to the next line when the next one does not
/// fit.
/// </summary>
/// <remarks>
/// <para>
/// This exists because WinUI 3 has no wrapping panel of its own that leaves its children as
/// ordinary controls with their own focus and tooltips. A <c>Panel</c> needs no default style,
/// so unlike a templated control it costs nothing to add.
/// </para>
/// <para>
/// It also removes the reason the key editor used to rebuild its own rows: doing that from a
/// <c>SizeChanged</c> handler modifies the visual tree during a layout pass, which the framework
/// refuses (and which crashed the window when it was first attempted), and doing it on a
/// deferred dispatcher turn touches elements that may already have been replaced. Letting the
/// layout system ask for a measure at the current width has neither problem: a resize is just
/// another measure.
/// </para>
/// </remarks>
public sealed class WrapPanel : Panel
{
    public double HorizontalSpacing { get; set; } = 6;

    public double VerticalSpacing { get; set; } = 6;

    protected override Size MeasureOverride(Size availableSize)
    {
        var limit = availableSize.Width;
        double x = 0;
        double y = 0;
        double rowHeight = 0;
        double widest = 0;

        foreach (var child in Children)
        {
            child.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var size = child.DesiredSize;
            if (x > 0 && x + size.Width > limit)
            {
                x = 0;
                y += rowHeight + VerticalSpacing;
                rowHeight = 0;
            }

            x += size.Width + HorizontalSpacing;
            widest = Math.Max(widest, x - HorizontalSpacing);
            rowHeight = Math.Max(rowHeight, size.Height);
        }

        var width = double.IsInfinity(limit) ? widest : Math.Min(limit, widest);
        return new Size(width, y + rowHeight);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        double x = 0;
        double y = 0;
        double rowHeight = 0;

        foreach (var child in Children)
        {
            var size = child.DesiredSize;
            if (x > 0 && x + size.Width > finalSize.Width)
            {
                x = 0;
                y += rowHeight + VerticalSpacing;
                rowHeight = 0;
            }

            child.Arrange(new Rect(x, y, size.Width, size.Height));
            x += size.Width + HorizontalSpacing;
            rowHeight = Math.Max(rowHeight, size.Height);
        }

        return finalSize;
    }
}
