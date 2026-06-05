using System;
using System.Globalization;
using System.Windows.Data;

namespace DevZip.App.Converters;

// Negates a boolean for one-way XAML bindings (e.g. enable a control only while
// the app is not busy).
public sealed class InverseBooleanConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is bool flag ? !flag : value;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => value is bool flag ? !flag : value;
}
