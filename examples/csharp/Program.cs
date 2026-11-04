// Device-side example in C#: bitmap -> WSQ + ANSI-378 ("MINEX") template, and a
// 1:1 match, by calling libfpx through P/Invoke. Works on Linux (libfpx.so),
// macOS (libfpx.dylib) and Windows (fpx.dll) with .NET 10.
//
//   cmake --build build
//   cd examples/csharp
//   LD_LIBRARY_PATH=../../build dotnet run -- ../../fpx-demo/finger1_0.bmp ../../fpx-demo/finger1_1.bmp
using System;
using System.IO;
using System.Runtime.InteropServices;

static class Fpx
{
    const string Lib = "fpx";  // resolves libfpx.so / libfpx.dylib / fpx.dll
    public const int FormatAnsi378 = 1, FormatIso19794 = 2;

    [DllImport(Lib)] static extern IntPtr fpx_version();
    [DllImport(Lib)] static extern IntPtr fpx_last_error();
    [DllImport(Lib)] static extern void fpx_free(IntPtr p);
    [DllImport(Lib)] static extern float fpx_default_threshold();
    [DllImport(Lib)] static extern int fpx_extract_from_image(byte[] image, UIntPtr imageLen, int dpi, int format,
                                                              out IntPtr outTemplate, out UIntPtr outLen);
    [DllImport(Lib)] static extern int fpx_image_to_wsq(byte[] image, UIntPtr imageLen, int dpi, float bitrate,
                                                        out IntPtr outWsq, out UIntPtr outLen);
    [DllImport(Lib)] static extern int fpx_match(byte[] a, UIntPtr aLen, byte[] b, UIntPtr bLen, out float score);

    public static string Version => Marshal.PtrToStringAnsi(fpx_version())!;
    public static float DefaultThreshold => fpx_default_threshold();

    static void Check(int rc)
    {
        if (rc != 0) throw new InvalidOperationException($"libfpx error {rc}: {Marshal.PtrToStringAnsi(fpx_last_error())}");
    }

    // Copies a libfpx-owned buffer into a managed array and frees it.
    static byte[] Take(IntPtr p, UIntPtr n)
    {
        try
        {
            var bytes = new byte[(int)n];
            Marshal.Copy(p, bytes, 0, bytes.Length);
            return bytes;
        }
        finally { fpx_free(p); }
    }

    /// Image bytes (BMP/PGM/TIFF/WSQ) -> template. dpi 0 = from the file, else 500.
    public static byte[] Extract(byte[] image, int dpi = 0, int format = FormatAnsi378)
    {
        Check(fpx_extract_from_image(image, (UIntPtr)image.Length, dpi, format, out var p, out var n));
        return Take(p, n);
    }

    public static byte[] ToWsq(byte[] image, int dpi = 0, float bitrate = 0.75f)
    {
        Check(fpx_image_to_wsq(image, (UIntPtr)image.Length, dpi, bitrate, out var p, out var n));
        return Take(p, n);
    }

    /// Two templates (ANSI or ISO) -> score in [0, 100].
    public static float Match(byte[] a, byte[] b)
    {
        Check(fpx_match(a, (UIntPtr)a.Length, b, (UIntPtr)b.Length, out var score));
        return score;
    }
}

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: dotnet run -- finger.bmp [other.bmp]");
            return 2;
        }
        Console.WriteLine($"libfpx {Fpx.Version}");
        var templates = new byte[args.Length][];
        for (int i = 0; i < args.Length; i++)
        {
            byte[] image = File.ReadAllBytes(args[i]);
            byte[] wsq = Fpx.ToWsq(image);
            templates[i] = Fpx.Extract(image);
            Console.WriteLine($"{args[i]}: {image.Length} B image -> {wsq.Length} B WSQ, {templates[i].Length} B ANSI-378 template");
        }
        if (templates.Length == 2)
        {
            float score = Fpx.Match(templates[0], templates[1]);
            string decision = score >= Fpx.DefaultThreshold ? "MATCH" : "NO MATCH";
            Console.WriteLine($"score {score:F1} (threshold {Fpx.DefaultThreshold:F0}) -> {decision}");
        }
        return 0;
    }
}
