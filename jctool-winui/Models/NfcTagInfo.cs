namespace JcTool.WinUI.Models;

public sealed class NfcTagInfo
{
    public required byte[] Uid { get; init; }
    public int TagType { get; init; }
    public int TagModel { get; init; }
    public required byte[] Data { get; init; }

    public bool IsNtag => TagType == 2;
}
