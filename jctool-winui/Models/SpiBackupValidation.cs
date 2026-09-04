namespace JcTool.WinUI.Models;

public sealed class SpiBackupValidation
{
    public bool IsValid { get; init; }
    public bool IsSameController { get; init; }
    public bool IsSameProduct { get; init; }
    public bool HasExpectedSize { get; init; }
    public bool HasChecksum { get; init; }
    public string Sha256 { get; init; } = string.Empty;
    public string MessageResource { get; init; } = "SpiBackupInvalid";
}
