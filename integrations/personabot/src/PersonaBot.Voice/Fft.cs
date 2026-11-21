namespace PersonaBot.Voice;

/// <summary>
/// A plain radix-2 complex FFT, in place, for the echo canceller's delay finder (EchoCanceller.FindDelay). The
/// length must be a power of two. Real and imaginary parts are kept in separate arrays.
/// </summary>
public static class Fft
{
    /// <summary>Transforms (<paramref name="re"/>, <paramref name="im"/>) in place; <paramref name="inverse"/> also divides by the length.</summary>
    public static void Transform(double[] re, double[] im, bool inverse)
    {
        ArgumentNullException.ThrowIfNull(re);
        ArgumentNullException.ThrowIfNull(im);
        var n = re.Length;
        if (n != im.Length) throw new ArgumentException("The real and imaginary parts must be the same length.", nameof(im));
        if (n == 0 || (n & (n - 1)) != 0) throw new ArgumentException("The length must be a power of two.", nameof(re));

        // Bit-reversed order.
        for (int i = 1, j = 0; i < n; i++)
        {
            var bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i >= j) continue;
            (re[i], re[j]) = (re[j], re[i]);
            (im[i], im[j]) = (im[j], im[i]);
        }

        for (var size = 2; size <= n; size <<= 1)
        {
            var angle = (inverse ? 2 : -2) * Math.PI / size;
            var stepRe = Math.Cos(angle);
            var stepIm = Math.Sin(angle);
            var half = size / 2;
            for (var start = 0; start < n; start += size)
            {
                double wRe = 1, wIm = 0;
                for (var k = 0; k < half; k++)
                {
                    var a = start + k;
                    var b = a + half;
                    var tRe = re[b] * wRe - im[b] * wIm;
                    var tIm = re[b] * wIm + im[b] * wRe;
                    re[b] = re[a] - tRe;
                    im[b] = im[a] - tIm;
                    re[a] += tRe;
                    im[a] += tIm;
                    var nextRe = wRe * stepRe - wIm * stepIm;
                    wIm = wRe * stepIm + wIm * stepRe;
                    wRe = nextRe;
                }
            }
        }

        if (!inverse) return;
        for (var i = 0; i < n; i++)
        {
            re[i] /= n;
            im[i] /= n;
        }
    }
}
