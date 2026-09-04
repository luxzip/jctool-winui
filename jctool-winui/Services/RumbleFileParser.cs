using System.Buffers.Binary;
using JcTool.WinUI.Models;

namespace JcTool.WinUI.Services;

public static class RumbleFileParser
{
    private static readonly float[] AmplitudeThresholds =
    {
        0.00000f, 0.007843f, 0.011823f, 0.014061f, 0.01672f, 0.019885f, 0.023648f, 0.028123f,
        0.033442f, 0.039771f, 0.047296f, 0.056246f, 0.066886f, 0.079542f, 0.094592f, 0.112491f,
        0.117471f, 0.122671f, 0.128102f, 0.133774f, 0.139697f, 0.145882f, 0.152341f, 0.159085f,
        0.166129f, 0.173484f, 0.181166f, 0.189185f, 0.197561f, 0.206308f, 0.215442f, 0.224982f,
        0.229908f, 0.234943f, 0.240087f, 0.245345f, 0.250715f, 0.256206f, 0.261816f, 0.267549f,
        0.273407f, 0.279394f, 0.285514f, 0.291765f, 0.298154f, 0.304681f, 0.311353f, 0.318171f,
        0.325138f, 0.332258f, 0.339534f, 0.346969f, 0.354566f, 0.362331f, 0.370265f, 0.378372f,
        0.386657f, 0.395124f, 0.403777f, 0.412619f, 0.421652f, 0.430885f, 0.440321f, 0.449964f,
        0.459817f, 0.469885f, 0.480174f, 0.490689f, 0.501433f, 0.512413f, 0.523633f, 0.535100f,
        0.546816f, 0.558790f, 0.571027f, 0.583530f, 0.596307f, 0.609365f, 0.622708f, 0.636344f,
        0.650279f, 0.664518f, 0.679069f, 0.693939f, 0.709133f, 0.724662f, 0.740529f, 0.756745f,
        0.773316f, 0.790249f, 0.807554f, 0.825237f, 0.843307f, 0.861772f, 0.880643f, 0.899928f,
        0.919633f, 0.939771f, 0.960348f, 0.981378f, 1.002867f
    };

    public static bool TryParse(byte[] file, out RumbleAsset? asset, out string errorResource)
    {
        asset = null;
        errorResource = "InvalidRumbleFile";
        if (file.Length >= 10 && file.AsSpan(0, 4).SequenceEqual("RRAW"u8))
        {
            var rate = BinaryPrimitives.ReadUInt16BigEndian(file.AsSpan(4, 2));
            var sampleCount = BinaryPrimitives.ReadUInt32BigEndian(file.AsSpan(6, 4));
            if (rate <= 0 || rate > 1000 || sampleCount == 0 || sampleCount > 10_000_000
                || 10UL + sampleCount * 4UL > (ulong)file.Length)
            {
                return false;
            }
            asset = new RumbleAsset
            {
                Samples = file.AsSpan(10, checked((int)sampleCount * 4)).ToArray(),
                SampleRateMilliseconds = rate,
                FormatResource = "RawRumbleFormat.Text"
            };
            return true;
        }

        if (file.Length < 12 || file[4] != 0x03)
        {
            return false;
        }
        var frequency = BinaryPrimitives.ReadUInt16LittleEndian(file.AsSpan(6, 2));
        if (frequency == 0)
        {
            return false;
        }
        var headerSize = file[0] switch
        {
            0x04 => 0x0c,
            0x0c when file.Length >= 0x14 => 0x14,
            0x10 when file.Length >= 0x18 => 0x18,
            _ => 0
        };
        if (headerSize == 0)
        {
            return false;
        }
        var dataSizeOffset = headerSize - 4;
        var dataSize = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(dataSizeOffset, 4));
        var loopStart = headerSize >= 0x14
            ? BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(8, 4))
            : 0;
        var loopEnd = headerSize >= 0x14
            ? BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(12, 4))
            : 0;
        var loopWait = headerSize >= 0x18
            ? BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(16, 4))
            : 0;
        var binarySampleCount = dataSize / 4;
        if (1000 / frequency <= 0 || 1000 / frequency > 1000 || dataSize == 0
            || dataSize % 4 != 0 || loopStart > loopEnd || loopEnd > binarySampleCount
            || (ulong)headerSize + dataSize > (ulong)file.Length)
        {
            return false;
        }

        var raw = file.AsSpan(headerSize, checked((int)dataSize)).ToArray();
        asset = new RumbleAsset
        {
            Samples = ConvertBinary(raw),
            SourceSamples = raw,
            SampleRateMilliseconds = 1000 / frequency,
            LoopStart = checked((int)loopStart),
            LoopEnd = checked((int)loopEnd),
            LoopWait = checked((int)loopWait),
            HasLoop = headerSize >= 0x14,
            FormatResource = headerSize switch
            {
                0x0c => "BinaryRumbleFormat.Text",
                0x14 => "LoopRumbleFormat.Text",
                _ => "LoopWaitRumbleFormat.Text"
            }
        };
        return true;
    }

    public static byte[] ConvertBinary(
        byte[] source,
        int lowAmplitudePercent = 100,
        int lowFrequencyPercent = 100,
        int highAmplitudePercent = 100,
        int highFrequencyPercent = 100)
    {
        if (source.Length == 0 || source.Length % 4 != 0)
        {
            throw new ArgumentException("Binary rumble data must contain four-byte samples.", nameof(source));
        }
        var output = new byte[source.Length];
        var lowGain = Math.Clamp(lowAmplitudePercent, 0, 200) / 100f;
        var lowPitch = Math.Clamp(lowFrequencyPercent, 0, 200) / 100f;
        var highGain = Math.Clamp(highAmplitudePercent, 0, 200) / 100f;
        var highPitch = Math.Clamp(highFrequencyPercent, 0, 200) / 100f;
        for (var offset = 0; offset < source.Length; offset += 4)
        {
            var lowAmplitude = Math.Clamp(source[offset] * lowGain, 0, 255);
            var lowFrequency = Math.Clamp(source[offset + 1] * lowPitch, 0, 191) - 0x40;
            var highAmplitude = Math.Clamp(source[offset + 2] * highGain, 0, 255);
            var sum = lowAmplitude / 255f + highAmplitude / 255f;
            if (sum > 1)
            {
                lowAmplitude /= sum;
                highAmplitude /= sum;
            }
            var highFrequency = (int)((Math.Clamp(source[offset + 3] * highPitch, 0, 223) - 0x60) * 4);
            var lowIndex = AmplitudeIndex(lowAmplitude / 255f);
            var highIndex = AmplitudeIndex(highAmplitude / 255f);
            var lowCode = (ushort)((lowIndex / 2 + 0x40) | (lowIndex % 2 == 0 ? 0 : 0x8000));
            var highCode = highFrequency;
            output[offset] = (byte)highCode;
            output[offset + 1] = (byte)((highCode >> 8) + highIndex * 2);
            output[offset + 2] = (byte)((lowCode >> 8) + lowFrequency);
            output[offset + 3] = (byte)lowCode;
        }
        return output;
    }

    private static int AmplitudeIndex(float amplitude)
    {
        for (var index = 1; index < AmplitudeThresholds.Length; index++)
        {
            if (amplitude < AmplitudeThresholds[index])
            {
                return index - 1;
            }
        }
        return AmplitudeThresholds.Length - 2;
    }
}
