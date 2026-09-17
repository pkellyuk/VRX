using System;
using System.Linq;
using System.Reflection;

var asm = typeof(Microsoft.ML.OnnxRuntime.InferenceSession).Assembly;
Console.WriteLine("assembly: " + asm.FullName);

foreach (var name in new[]
{
    "Microsoft.ML.OnnxRuntime.OrtValue",
    "Microsoft.ML.OnnxRuntime.NodeMetadata",
    "Microsoft.ML.OnnxRuntime.TensorElementType",
    "Microsoft.ML.OnnxRuntime.Float16",
    "Microsoft.ML.OnnxRuntime.SessionOptions",
})
{
    var t = asm.GetType(name);
    Console.WriteLine();
    Console.WriteLine("===== " + name + " =====");
    if (t == null)
    {
        Console.WriteLine("  NOT FOUND in this assembly");
        continue;
    }

    foreach (var m in t.GetMembers(BindingFlags.Public | BindingFlags.Instance)
                       .Select(m => m.ToString())
                       .Where(s => s != null)
                       .Distinct()
                       .OrderBy(s => s))
    {
        Console.WriteLine("  " + m);
    }
}

// Anything else that looks like a tensor/value helper
Console.WriteLine();
Console.WriteLine("===== types mentioning Tensor/Value/Metadata =====");
foreach (var t in asm.GetTypes()
                    .Where(t => t.IsPublic && (t.Name.Contains("Tensor") || t.Name.Contains("Value") || t.Name.Contains("Metadata")))
                    .OrderBy(t => t.FullName))
{
    Console.WriteLine("  " + t.FullName);
}
