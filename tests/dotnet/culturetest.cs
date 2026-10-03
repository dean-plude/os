// .NET globalization on NovaOS: formats, names and compares text in German
// and Japanese, the way .NET does it through ICU (C:\Windows\System32\icu.dll).
// Prints one "name: value" line per check and "culturetest: ok" when every
// check matches what Windows 10 prints; the exit code is the number of
// mismatches.  Prebuilt as culturetest.dll (see tests/dotnet/README.md).
using System;
using System.Globalization;
using System.Reflection;

static class CultureTest
{
    static int failed;

    static void Check(string name, string got, string want)
    {
        bool ok = got == want;
        Console.WriteLine($"{name}: {got}" + (ok ? "" : $"   <-- expected {want}"));
        if (!ok) failed++;
    }

    static int Main()
    {
        // which globalization back end .NET picked (internal, so by reflection)
        var mode = typeof(object).Assembly.GetType("System.Globalization.GlobalizationMode");
        var useNls = mode?.GetProperty("UseNls", BindingFlags.Static | BindingFlags.NonPublic);
        var invariant = mode?.GetProperty("Invariant", BindingFlags.Static | BindingFlags.NonPublic);
        string backend = (bool)(invariant?.GetValue(null) ?? false) ? "invariant"
                       : (bool)(useNls?.GetValue(null) ?? false) ? "NLS" : "ICU";
        Check("globalization", backend, "ICU");

        var de = new CultureInfo("de-DE");
        var ja = new CultureInfo("ja-JP");
        var day = new DateTime(2026, 10, 2, 14, 5, 9);

        Check("de-DE name", de.NativeName, "Deutsch (Deutschland)");
        Check("de-DE number", 1234567.891.ToString("N2", de), "1.234.567,89");
        Check("de-DE currency", 1234.5m.ToString("C", de), "1.234,50 \u20ac");
        Check("de-DE long date", day.ToString("D", de), "Freitag, 2. Oktober 2026");
        Check("de-DE short date", day.ToString("d", de), "02.10.2026");
        Check("de-DE time", day.ToString("T", de), "14:05:09");
        Check("de-DE month", de.DateTimeFormat.GetMonthName(3), "M\u00e4rz");
        Check("de-DE parse", double.Parse("1.234,5", de).ToString(CultureInfo.InvariantCulture), "1234.5");
        Check("de-DE compare", Math.Sign(string.Compare("\u00e4b", "az", de, CompareOptions.None)).ToString(), "-1");
        Check("de-DE accents", string.Compare("M\u00fcller", "Muller", de, CompareOptions.IgnoreNonSpace).ToString(), "0");

        Check("ja-JP name", ja.NativeName, "\u65e5\u672c\u8a9e (\u65e5\u672c)");
        Check("ja-JP number", 1234567.891.ToString("N2", ja), "1,234,567.89");
        Check("ja-JP currency", 1234.5m.ToString("C", ja), "\uffe51,235");
        Check("ja-JP long date", day.ToString("D", ja), "2026\u5e7410\u67082\u65e5\u91d1\u66dc\u65e5");
        Check("ja-JP short date", day.ToString("d", ja), "2026/10/02");
        Check("ja-JP day", ja.DateTimeFormat.GetDayName(DayOfWeek.Friday), "\u91d1\u66dc\u65e5");
        Check("ja-JP kana", string.Compare("\u3042", "\u30a2", ja, CompareOptions.IgnoreKanaType).ToString(), "0");
        Check("ja-JP width", string.Compare("\uff21", "A", ja, CompareOptions.IgnoreWidth).ToString(), "0");

        Check("tr-TR upper", "i".ToUpper(new CultureInfo("tr-TR")), "\u0130");
        Check("fr-FR name", new CultureInfo("fr-FR").DisplayName.Length > 0 ? "ok" : "", "ok");

        Console.WriteLine(failed == 0 ? "culturetest: ok" : $"culturetest: {failed} failed");
        return failed;
    }
}
