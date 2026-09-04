namespace JcTool.WinUI.Models;

public sealed class DiagnosticReply
{
    public required byte[] Sent { get; init; }
    public required byte[] Received { get; init; }
    public bool Matched { get; init; }
    public bool Accepted { get; init; }
}
