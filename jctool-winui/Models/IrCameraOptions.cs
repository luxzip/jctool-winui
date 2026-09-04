namespace JcTool.WinUI.Models;

public enum IrColorMode
{
    Grayscale,
    NightVision,
    Infrared
}

public sealed class IrCameraOptions
{
    public int Resolution { get; init; }
    public int ExposureMicroseconds { get; init; } = 200;
    public int DigitalGain { get; init; } = 1;
    public bool LedsEnabled { get; init; } = true;
    public bool ExternalLightFilter { get; init; } = true;
    public bool FlipHorizontal { get; init; }
    public bool Denoise { get; init; } = true;
    public IrColorMode ColorMode { get; init; } = IrColorMode.Grayscale;

    public int Width => Resolution switch { 0 => 40, 1 => 80, 2 => 160, _ => 320 };
    public int Height => Resolution switch { 0 => 30, 1 => 60, 2 => 120, _ => 240 };
}
