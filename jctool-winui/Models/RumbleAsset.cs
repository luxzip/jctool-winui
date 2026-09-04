namespace JcTool.WinUI.Models;

public sealed class RumbleAsset
{
    public required byte[] Samples { get; init; }
    public byte[]? SourceSamples { get; init; }
    public int SampleRateMilliseconds { get; init; }
    public int LoopStart { get; init; }
    public int LoopEnd { get; init; }
    public int LoopWait { get; init; }
    public bool HasLoop { get; init; }
    public string FormatResource { get; init; } = "RawRumbleFormat.Text";
    public int SourceSampleCount => Samples.Length / 4;

    public int TotalSampleCount(int loopTimes)
    {
        if (!HasLoop)
        {
            return SourceSampleCount;
        }
        return checked(LoopStart
            + (LoopEnd - LoopStart) * (1 + loopTimes)
            + (SourceSampleCount - LoopEnd)
            + LoopWait * (1 + loopTimes));
    }

    public byte[] Expand(int loopTimes)
    {
        if (!HasLoop)
        {
            return Samples.ToArray();
        }
        var count = TotalSampleCount(loopTimes);
        var output = new byte[checked(count * 4)];
        var position = 0;
        CopyFrames(output, ref position, Samples, 0, LoopStart);
        for (var loop = 0; loop <= Math.Max(0, loopTimes); loop++)
        {
            CopyFrames(output, ref position, Samples, LoopStart, LoopEnd - LoopStart);
            if (LoopWait > 0)
            {
                position += LoopWait * 4;
            }
        }
        CopyFrames(output, ref position, Samples, LoopEnd, SourceSampleCount - LoopEnd);
        return output;
    }

    private static void CopyFrames(
        byte[] destination,
        ref int destinationOffset,
        byte[] source,
        int startFrame,
        int frameCount)
    {
        var byteCount = checked(frameCount * 4);
        Buffer.BlockCopy(source, checked(startFrame * 4), destination, destinationOffset, byteCount);
        destinationOffset += byteCount;
    }
}
