using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

// M1 probe: native ONNX Runtime + DirectML, warm session, no Python in the loop.
// Measures per-run inference latency percentiles of DA-V2-Small.
//
// usage: ortprobe <model.onnx> [size] [warmup] [iters] [key=value ...]
//   extra key=value pairs are passed to SessionOptions.AddSessionConfigEntry,
//   e.g. ep.dml.enable_graph_capture=1

class Program
{
    static double Checksum(float[] buf)
    {
        double sum = 0;
        foreach (var v in buf) sum += v;
        return sum;
    }

    static int Main(string[] args)
    {
        string modelPath = args.Length > 0
            ? args[0]
            : @"C:\Users\paulj\dev\VRX\bench\models\onnx-community-depth-anything-v2-small\onnx\model_fp16.onnx";
        int size = args.Length > 1 ? int.Parse(args[1]) : 518;
        int warmup = args.Length > 2 ? int.Parse(args[2]) : 10;
        int iters = args.Length > 3 ? int.Parse(args[3]) : 50;

        if (!File.Exists(modelPath))
        {
            Console.WriteLine("MODEL NOT FOUND: " + modelPath);
            return 2;
        }

        Console.WriteLine($"model : {Path.GetFileName(modelPath)}");
        Console.WriteLine($"input : {size}x{size}   runs: {warmup} warmup + {iters} timed");

        var so = new SessionOptions();
        so.GraphOptimizationLevel = GraphOptimizationLevel.ORT_ENABLE_ALL;

        var cfg = new List<string>();
        for (int i = 4; i < args.Length; i++)
        {
            int eq = args[i].IndexOf('=');
            if (eq <= 0) continue;
            string k = args[i].Substring(0, eq);
            string v = args[i].Substring(eq + 1);
            so.AddSessionConfigEntry(k, v);
            cfg.Add(args[i]);
        }

        string epName = "CPU";
        try
        {
            so.AppendExecutionProvider_DML(0);
            epName = "DirectML";
        }
        catch (Exception ex)
        {
            Console.WriteLine("DML EP unavailable -> " + ex.GetType().Name + ": " + ex.Message);
        }

        var swLoad = Stopwatch.StartNew();
        using var session = new InferenceSession(modelPath, so);
        swLoad.Stop();

        var inputMeta = session.InputMetadata.First();
        var outputMeta = session.OutputMetadata.First();
        var elem = inputMeta.Value.ElementDataType;
        var outElem = outputMeta.Value.ElementDataType;

        Console.WriteLine($"EP    : {epName}   load: {swLoad.Elapsed.TotalMilliseconds:F0} ms");
        Console.WriteLine($"config: {(cfg.Count == 0 ? "(defaults)" : string.Join(" ", cfg))}");
        Console.WriteLine($"in/out: '{inputMeta.Key}' {elem} -> '{outputMeta.Key}' {outElem}");

        int n = 1 * 3 * size * size;
        var rng = new Random(1234);

        float[]? inFloat = null;
        Float16[]? inHalf = null;
        OrtValue inputValue;
        if (elem == TensorElementType.Float16)
        {
            inHalf = new Float16[n];
            for (int i = 0; i < n; i++)
                inHalf[i] = new Float16(BitConverter.HalfToUInt16Bits((Half)(float)rng.NextDouble()));
            inputValue = OrtValue.CreateTensorValueFromMemory(inHalf, new long[] { 1, 3, size, size });
        }
        else
        {
            inFloat = new float[n];
            for (int i = 0; i < n; i++) inFloat[i] = (float)rng.NextDouble();
            inputValue = OrtValue.CreateTensorValueFromMemory(inFloat, new long[] { 1, 3, size, size });
        }

        var inputs = new Dictionary<string, OrtValue> { { inputMeta.Key, inputValue } };
        var runOpts = new RunOptions();

        bool prealloc = cfg.Any(c => c.IndexOf("graph_capture", StringComparison.OrdinalIgnoreCase) >= 0);
        float[]? outBuf = null;
        OrtValue? outValue = null;
        if (prealloc)
        {
            outBuf = new float[1 * size * size];
            outValue = OrtValue.CreateTensorValueFromMemory(outBuf, new long[] { 1, size, size });
        }

        void OneRun()
        {
            if (prealloc)
                session.Run(runOpts, session.InputNames, new[] { inputValue },
                                     session.OutputNames, new[] { outValue! });
            else
                using (var r = session.Run(runOpts, inputs, session.OutputNames)) { }
        }

        Console.Write("warmup: ");
        for (int i = 0; i < warmup; i++) { OneRun(); Console.Write("."); }
        Console.WriteLine(" done");

        var samples = new double[iters];
        var total = Stopwatch.StartNew();
        for (int i = 0; i < iters; i++)
        {
            var sw = Stopwatch.StartNew();
            OneRun();
            // force a result to exist
            if (prealloc) { double s = outBuf![0]; if (s == double.NaN) Console.Write(""); }
            sw.Stop();
            samples[i] = sw.Elapsed.TotalMilliseconds;
        }
        total.Stop();

        Array.Sort(samples);
        double P(double q) => samples[Math.Min(samples.Length - 1, (int)(q * samples.Length))];

        // ---------- correctness: does the output actually change with the input? ----------
        float[] ReadOutput()
        {
            if (prealloc) return (float[])outBuf!.Clone();
            using var rv = session.Run(runOpts, inputs, session.OutputNames);
            return rv.First().GetTensorDataAsSpan<float>().ToArray();
        }

        OneRun();
        var a = ReadOutput();

        // Change the input, in place, to a constant. Nothing else changes.
        if (inFloat != null) Array.Fill(inFloat, 1.0f);
        else if (inHalf != null)
            for (int i = 0; i < inHalf.Length; i++) inHalf[i] = new Float16(BitConverter.HalfToUInt16Bits((Half)1.0f));

        OneRun();
        var b = ReadOutput();

        bool live = false;
        for (int i = 0; i < a.Length; i++)
            if (Math.Abs(a[i] - b[i]) > 1e-4) { live = true; break; }

        Console.WriteLine();
        Console.WriteLine($"p50   : {P(0.50):F2} ms   ({1000.0 / P(0.50):F1} fps)");
        Console.WriteLine($"mean  : {samples.Average():F2} ms    p95: {P(0.95):F2} ms    min: {samples[0]:F2} ms");
        Console.WriteLine($"budget: 90Hz=11.1ms 120Hz=8.3ms 144Hz=6.9ms");
        Console.WriteLine();
        Console.WriteLine($"sum(random input) : {Checksum(a):F3}");
        Console.WriteLine($"sum(const  input) : {Checksum(b):F3}");
        Console.WriteLine($"LIVE INPUT        : {(live ? "YES - output tracks input (usable)" : "NO - output did not change (STALE/BROKEN)")}");
        Console.WriteLine($"VERDICT           : {(P(0.50) < 11.1 ? "fits 90 Hz" : "over 90 Hz")}{(live ? "" : "  [timing NOT trustworthy]")}");

        inputValue.Dispose();
        return 0;
    }
}
