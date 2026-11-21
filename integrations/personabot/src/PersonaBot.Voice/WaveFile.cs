namespace PersonaBot.Voice;

/// <summary>Reads and writes plain PCM wave files (for the helper's test modes).</summary>
public static class WaveFile
{
    /// <summary>Writes mono float samples as a 16-bit wave file.</summary>
    public static void Write(string file, float[] samples, int sampleRate)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(file);
        ArgumentNullException.ThrowIfNull(samples);
        using var writer = new BinaryWriter(File.Create(file));
        writer.Write("RIFF"u8);
        writer.Write(36 + samples.Length * 2);
        writer.Write("WAVEfmt "u8);
        writer.Write(16);
        writer.Write((short)1);
        writer.Write((short)1);
        writer.Write(sampleRate);
        writer.Write(sampleRate * 2);
        writer.Write((short)2);
        writer.Write((short)16);
        writer.Write("data"u8);
        writer.Write(samples.Length * 2);
        foreach (var s in samples) writer.Write((short)Math.Clamp(s * 32767f, -32768f, 32767f));
    }

    /// <summary>Reads a 16-bit PCM wave file (mono, or the first channel) as floats.</summary>
    public static (float[] Samples, int SampleRate) Read(string file)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(file);
        using var reader = new BinaryReader(File.OpenRead(file));
        if (new string(reader.ReadChars(4)) != "RIFF") throw new InvalidDataException("Not a wave file: " + file);
        reader.ReadInt32();
        if (new string(reader.ReadChars(4)) != "WAVE") throw new InvalidDataException("Not a wave file: " + file);
        int channels = 1, rate = 16000, bits = 16;
        while (reader.BaseStream.Position < reader.BaseStream.Length)
        {
            var id = new string(reader.ReadChars(4));
            var size = reader.ReadInt32();
            if (id == "fmt ")
            {
                reader.ReadInt16();
                channels = reader.ReadInt16();
                rate = reader.ReadInt32();
                reader.ReadInt32();
                reader.ReadInt16();
                bits = reader.ReadInt16();
                reader.BaseStream.Seek(size - 16, SeekOrigin.Current);
                continue;
            }
            if (id != "data")
            {
                reader.BaseStream.Seek(size, SeekOrigin.Current);
                continue;
            }
            if (bits != 16) throw new InvalidDataException($"Only 16-bit wave files, not {bits}-bit.");
            var frames = size / 2 / channels;
            var samples = new float[frames];
            for (var i = 0; i < frames; i++)
            {
                samples[i] = reader.ReadInt16() / 32768f;
                for (var c = 1; c < channels; c++) reader.ReadInt16();
            }
            return (samples, rate);
        }
        throw new InvalidDataException("No audio in " + file);
    }
}
