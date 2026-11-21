using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Automaton.Core.Services.ComputerUse;

/// <summary>One recognised word, with its box in the pixels of the image that was read.</summary>
public sealed class OcrTextWord
{
    public string Text { get; set; } = string.Empty;

    public RectangleF Bounds { get; set; }
}

/// <summary>One recognised line: its words in reading order.</summary>
public sealed class OcrTextLine
{
    public string Text { get; set; } = string.Empty;

    public IReadOnlyList<OcrTextWord> Words { get; set; } = Array.Empty<OcrTextWord>();
}

/// <summary>
/// Windows' built-in OCR (<c>Windows.Media.Ocr</c>), called directly.
///
/// Built in, so nothing to install or ship, and it reads pixels, so it works where the
/// accessibility tree is empty: Steam's library exposed four unnamed panes to UI
/// Automation, and Edge exposed its toolbar but none of the page, while every target in
/// both was a text label on screen.
///
/// Called through the raw WinRT ABI rather than the C# projection, because the
/// projection needs a Windows-versioned target framework and Core, like Tools and the
/// headless host that load it, targets plain net10.0. Every interface slot and IID
/// below was read out of the Windows metadata files (System32\WinMetadata), not written
/// from memory: a wrong slot here is not an exception, it is a call into the wrong
/// function.
/// </summary>
[SupportedOSPlatform("windows")]
public static class WindowsOcr
{
    private const int BgraPixelFormat = 87;
    private const int AsyncStarted = 0;
    private const int AsyncCompleted = 1;
    private const int RoInitMultithreaded = 1;
    private const int RpcEChangedMode = unchecked((int)0x80010106);

    private static readonly TimeSpan RecognitionTimeout = TimeSpan.FromSeconds(20);

    private static Guid IidOcrEngineStatics = new("5bffa85a-3384-3540-9940-699120d428a8");
    private static Guid IidSoftwareBitmapStatics = new("df0385db-672f-4a9d-806e-c2442f343e86");
    private static Guid IidCryptographicBufferStatics = new("320b7e22-3cb0-4cdf-8663-1d28910065eb");
    private static Guid IidAsyncInfo = new("00000036-0000-0000-c000-000000000046");
    private static Guid IidClosable = new("30d5a829-7fa4-4026-83bb-d75bae4ea99e");

    // Slots, counting IUnknown (0-2) and IInspectable (3-5) first.
    private const int OcrEngineStatics_MaxImageDimension = 6;
    private const int OcrEngineStatics_TryCreateFromUserProfileLanguages = 10;
    private const int OcrEngine_RecognizeAsync = 6;
    private const int CryptographicBufferStatics_CreateFromByteArray = 9;
    private const int SoftwareBitmapStatics_CreateCopyFromBuffer = 9; // (source, format, width, height)
    private const int AsyncInfo_Status = 7;
    private const int AsyncInfo_ErrorCode = 8;
    private const int AsyncOperation_GetResults = 8;
    private const int OcrResult_Lines = 6;
    private const int VectorView_GetAt = 6;
    private const int VectorView_Size = 7;
    private const int OcrLine_Words = 6;
    private const int OcrLine_Text = 7;
    private const int OcrWord_BoundingRect = 6;
    private const int OcrWord_Text = 7;
    private const int Closable_Close = 6;

    /// <summary>
    /// The largest width or height the engine accepts. Images beyond it must be scaled
    /// down by the caller.
    /// </summary>
    public static int MaxImageDimension()
    {
        EnsureWinRt();

        var statics = GetFactory("Windows.Media.Ocr.OcrEngine", ref IidOcrEngineStatics);
        try
        {
            Check(Slot<GetUInt32>(statics, OcrEngineStatics_MaxImageDimension)(statics, out var value), "reading OcrEngine.MaxImageDimension");
            return (int)value;
        }
        finally
        {
            Marshal.Release(statics);
        }
    }

    /// <summary>
    /// Reads all the text in a bitmap, in the user's own languages.
    /// </summary>
    /// <exception cref="InvalidOperationException">No OCR language is installed for the user's languages.</exception>
    public static IReadOnlyList<OcrTextLine> Recognize(Bitmap bitmap)
    {
        ArgumentNullException.ThrowIfNull(bitmap);

        AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[WindowsOcr] Recognize: Entry - {bitmap.Width}x{bitmap.Height}");

        EnsureWinRt();

        var clock = Stopwatch.StartNew();
        var pixels = ReadBgra(bitmap);

        IntPtr engineStatics = IntPtr.Zero, engine = IntPtr.Zero;
        IntPtr bufferStatics = IntPtr.Zero, buffer = IntPtr.Zero;
        IntPtr bitmapStatics = IntPtr.Zero, softwareBitmap = IntPtr.Zero;
        IntPtr operation = IntPtr.Zero, asyncInfo = IntPtr.Zero, result = IntPtr.Zero;

        try
        {
            engineStatics = GetFactory("Windows.Media.Ocr.OcrEngine", ref IidOcrEngineStatics);
            Check(Slot<GetPointer>(engineStatics, OcrEngineStatics_TryCreateFromUserProfileLanguages)(engineStatics, out engine), "creating the OCR engine");

            if (engine == IntPtr.Zero)
            {
                throw new InvalidOperationException(
                    "Windows OCR has no recogniser for this user's languages. Add an OCR language under Settings > Time & language > Language & region (a language's optional 'Optical character recognition' feature).");
            }

            bufferStatics = GetFactory("Windows.Security.Cryptography.CryptographicBuffer", ref IidCryptographicBufferStatics);
            Check(Slot<CreateFromByteArray>(bufferStatics, CryptographicBufferStatics_CreateFromByteArray)(bufferStatics, (uint)pixels.Length, pixels, out buffer), "wrapping the pixels in a buffer");

            bitmapStatics = GetFactory("Windows.Graphics.Imaging.SoftwareBitmap", ref IidSoftwareBitmapStatics);
            Check(Slot<CreateCopyFromBuffer>(bitmapStatics, SoftwareBitmapStatics_CreateCopyFromBuffer)(bitmapStatics, buffer, BgraPixelFormat, bitmap.Width, bitmap.Height, out softwareBitmap), "creating the software bitmap");

            Check(Slot<RecognizeAsync>(engine, OcrEngine_RecognizeAsync)(engine, softwareBitmap, out operation), "starting recognition");

            asyncInfo = QueryInterface(operation, ref IidAsyncInfo, "the recognition operation's IAsyncInfo");
            WaitForCompletion(asyncInfo);

            Check(Slot<GetPointer>(operation, AsyncOperation_GetResults)(operation, out result), "reading the recognition result");

            var lines = ReadLines(result);

            AutomatonDebug.Log(AutomatonDebug.DEBUG_INFO, $"[WindowsOcr] Recognize: Exit - {lines.Count} line(s) in {clock.ElapsedMilliseconds}ms");
            return lines;
        }
        finally
        {
            Close(softwareBitmap);
            Release(result);
            Release(asyncInfo);
            Release(operation);
            Release(softwareBitmap);
            Release(bitmapStatics);
            Release(buffer);
            Release(bufferStatics);
            Release(engine);
            Release(engineStatics);
        }
    }

    private static List<OcrTextLine> ReadLines(IntPtr result)
    {
        var lines = new List<OcrTextLine>();

        Check(Slot<GetPointer>(result, OcrResult_Lines)(result, out var lineVector), "reading the recognised lines");

        try
        {
            Check(Slot<GetUInt32>(lineVector, VectorView_Size)(lineVector, out var lineCount), "counting lines");

            for (uint i = 0; i < lineCount; i++)
            {
                Check(Slot<GetPointerAt>(lineVector, VectorView_GetAt)(lineVector, i, out var line), "reading a line");

                try
                {
                    lines.Add(new OcrTextLine
                    {
                        Text = ReadString(line, OcrLine_Text),
                        Words = ReadWords(line)
                    });
                }
                finally
                {
                    Marshal.Release(line);
                }
            }
        }
        finally
        {
            Marshal.Release(lineVector);
        }

        return lines;
    }

    private static List<OcrTextWord> ReadWords(IntPtr line)
    {
        var words = new List<OcrTextWord>();

        Check(Slot<GetPointer>(line, OcrLine_Words)(line, out var wordVector), "reading a line's words");

        try
        {
            Check(Slot<GetUInt32>(wordVector, VectorView_Size)(wordVector, out var wordCount), "counting words");

            for (uint i = 0; i < wordCount; i++)
            {
                Check(Slot<GetPointerAt>(wordVector, VectorView_GetAt)(wordVector, i, out var word), "reading a word");

                try
                {
                    Check(Slot<GetRect>(word, OcrWord_BoundingRect)(word, out var rect), "reading a word's box");

                    words.Add(new OcrTextWord
                    {
                        Text = ReadString(word, OcrWord_Text),
                        Bounds = new RectangleF(rect.X, rect.Y, rect.Width, rect.Height)
                    });
                }
                finally
                {
                    Marshal.Release(word);
                }
            }
        }
        finally
        {
            Marshal.Release(wordVector);
        }

        return words;
    }

    /// <summary>
    /// Polls rather than registering a completion handler. A handler would be a COM
    /// object implemented in managed code; polling a status is a plain call, and
    /// recognition takes long enough that a few milliseconds of latency are nothing.
    /// </summary>
    private static void WaitForCompletion(IntPtr asyncInfo)
    {
        var clock = Stopwatch.StartNew();
        var getStatus = Slot<GetInt32>(asyncInfo, AsyncInfo_Status);

        while (true)
        {
            Check(getStatus(asyncInfo, out var status), "polling recognition");

            if (status == AsyncCompleted)
            {
                return;
            }

            if (status != AsyncStarted)
            {
                Slot<GetInt32>(asyncInfo, AsyncInfo_ErrorCode)(asyncInfo, out var errorCode);
                throw new InvalidOperationException($"Windows OCR failed (status {status}, HRESULT 0x{errorCode:X8}).");
            }

            if (clock.Elapsed > RecognitionTimeout)
            {
                throw new TimeoutException($"Windows OCR did not finish within {RecognitionTimeout.TotalSeconds:0} seconds.");
            }

            Thread.Sleep(10);
        }
    }

    /// <summary>
    /// The bitmap's pixels as tightly packed BGRA rows, which is the layout both GDI+
    /// 32bpp ARGB and WinRT Bgra8 use in memory.
    /// </summary>
    private static byte[] ReadBgra(Bitmap bitmap)
    {
        using var copy = bitmap.PixelFormat == PixelFormat.Format32bppArgb
            ? null
            : bitmap.Clone(new Rectangle(0, 0, bitmap.Width, bitmap.Height), PixelFormat.Format32bppArgb);
        var source = copy ?? bitmap;

        var data = source.LockBits(new Rectangle(0, 0, source.Width, source.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);

        try
        {
            var rowBytes = source.Width * 4;
            var pixels = new byte[rowBytes * source.Height];

            for (var y = 0; y < source.Height; y++)
            {
                Marshal.Copy(data.Scan0 + y * data.Stride, pixels, y * rowBytes, rowBytes);
            }

            return pixels;
        }
        finally
        {
            source.UnlockBits(data);
        }
    }

    private static string ReadString(IntPtr instance, int slot)
    {
        Check(Slot<GetPointer>(instance, slot)(instance, out var hstring), "reading text");

        try
        {
            var raw = WindowsGetStringRawBuffer(hstring, out var length);
            return raw == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUni(raw, (int)length);
        }
        finally
        {
            WindowsDeleteString(hstring);
        }
    }

    private static void EnsureWinRt()
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Windows OCR is only available on Windows.");
        }

        var hr = RoInitialize(RoInitMultithreaded);

        // Already initialised, in either apartment, is fine: WinRT activation works from
        // both, and the OCR objects are agile.
        if (hr < 0 && hr != RpcEChangedMode)
        {
            Check(hr, "initialising the Windows Runtime");
        }
    }

    private static IntPtr GetFactory(string className, ref Guid iid)
    {
        Check(WindowsCreateString(className, className.Length, out var hstring), $"naming {className}");

        try
        {
            Check(RoGetActivationFactory(hstring, ref iid, out var factory), $"getting the factory for {className}");
            return factory;
        }
        finally
        {
            WindowsDeleteString(hstring);
        }
    }

    private static IntPtr QueryInterface(IntPtr instance, ref Guid iid, string what)
    {
        Check(Marshal.QueryInterface(instance, in iid, out var result), $"getting {what}");
        return result;
    }

    private static void Close(IntPtr instance)
    {
        if (instance == IntPtr.Zero)
        {
            return;
        }

        if (Marshal.QueryInterface(instance, in IidClosable, out var closable) >= 0)
        {
            Slot<NoArguments>(closable, Closable_Close)(closable);
            Marshal.Release(closable);
        }
    }

    private static void Release(IntPtr instance)
    {
        if (instance != IntPtr.Zero)
        {
            Marshal.Release(instance);
        }
    }

    private static T Slot<T>(IntPtr instance, int slot) where T : Delegate
    {
        var vtable = Marshal.ReadIntPtr(instance);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return Marshal.GetDelegateForFunctionPointer<T>(function);
    }

    private static void Check(int hr, string what)
    {
        if (hr >= 0)
        {
            return;
        }

        throw new InvalidOperationException($"Windows OCR failed while {what} (HRESULT 0x{hr:X8}).", Marshal.GetExceptionForHR(hr));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WinRtRect
    {
        public float X;
        public float Y;
        public float Width;
        public float Height;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetUInt32(IntPtr self, out uint value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetInt32(IntPtr self, out int value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetPointer(IntPtr self, out IntPtr value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetPointerAt(IntPtr self, uint index, out IntPtr value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetRect(IntPtr self, out WinRtRect value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int NoArguments(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int CreateFromByteArray(IntPtr self, uint length, byte[] value, out IntPtr buffer);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int CreateCopyFromBuffer(IntPtr self, IntPtr source, int format, int width, int height, out IntPtr bitmap);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int RecognizeAsync(IntPtr self, IntPtr bitmap, out IntPtr operation);

    [DllImport("combase.dll")]
    private static extern int RoInitialize(int initType);

    [DllImport("combase.dll")]
    private static extern int RoGetActivationFactory(IntPtr activatableClassId, ref Guid iid, out IntPtr factory);

    [DllImport("combase.dll", CharSet = CharSet.Unicode)]
    private static extern int WindowsCreateString(string sourceString, int length, out IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern int WindowsDeleteString(IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern IntPtr WindowsGetStringRawBuffer(IntPtr hstring, out uint length);
}
