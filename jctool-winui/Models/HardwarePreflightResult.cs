namespace JcTool.WinUI.Models;

public sealed class HardwarePreflightResult
{
    public int ExitCode { get; init; }
    public string StepResource { get; init; } = "HardwarePreflightUnknownStep";
    public bool Succeeded => ExitCode == 0;
}
