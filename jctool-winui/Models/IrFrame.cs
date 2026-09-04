namespace JcTool.WinUI.Models;

public sealed class IrFrame
{
    public required byte[] Pixels { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int AverageIntensity { get; init; }
    public int WhitePixels { get; init; }
    public int AmbientPixels { get; init; }
    public uint FrameNumber { get; init; }
}
