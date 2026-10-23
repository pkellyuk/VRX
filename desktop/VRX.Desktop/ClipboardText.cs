using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;

namespace VRX.Desktop;

// Puts text on the Windows clipboard. Another program (clipboard history, a sync or remote
// desktop tool) can hold the clipboard open for a moment, which makes WPF throw
// CLIPBRD_E_CANT_OPEN; it is retried for about half a second. WPF can also throw after the
// text was already placed (only the final flush failed), so a failure that left the text on
// the clipboard counts as copied.
internal static class ClipboardText
{
    private const int Attempts = 10;
    private const int RetryDelayMs = 50;

    // true when the clipboard holds `text`; false when Windows would not let VRX use it.
    internal static async Task<bool> Copy(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        Debug.WriteLine($"[Clipboard] Copy enter: {text.Length} characters");
        for (int attempt = 1; attempt <= Attempts; ++attempt)
        {
            try
            {
                Clipboard.SetDataObject(text, true);
                Debug.WriteLine($"[Clipboard] Copy exit: copied on attempt {attempt}");
                return true;
            }
            catch (ExternalException ex)
            {
                Debug.WriteLine($"[Clipboard] Copy attempt {attempt} of {Attempts}: 0x{ex.ErrorCode:X8} {ex.Message}");
            }
            if (Holds(text))
            {
                Debug.WriteLine($"[Clipboard] Copy exit: the text is on the clipboard despite the error (attempt {attempt})");
                return true;
            }
            await Task.Delay(RetryDelayMs);
        }
        Debug.WriteLine("[Clipboard] Copy exit: the clipboard stayed in use by another program");
        return false;
    }

    private static bool Holds(string text)
    {
        try
        {
            return Clipboard.ContainsText() && Clipboard.GetText() == text;
        }
        catch (ExternalException ex)
        {
            Debug.WriteLine($"[Clipboard] Holds: could not read the clipboard: 0x{ex.ErrorCode:X8}");
            return false;
        }
    }
}
