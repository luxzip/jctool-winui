namespace JcTool.WinUI.Models;

public sealed class InputSnapshot
{
    public uint Buttons { get; init; }
    public int LeftX { get; init; }
    public int LeftY { get; init; }
    public int RightX { get; init; }
    public int RightY { get; init; }
    public short AccelerationX { get; init; }
    public short AccelerationY { get; init; }
    public short AccelerationZ { get; init; }
    public short GyroscopeX { get; init; }
    public short GyroscopeY { get; init; }
    public short GyroscopeZ { get; init; }
    public int ConnectionType { get; init; }
    public int BatteryLevel { get; init; }
    public bool Charging { get; init; }
}
