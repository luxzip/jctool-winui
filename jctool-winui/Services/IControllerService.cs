using JcTool.WinUI.Models;

namespace JcTool.WinUI.Services;

public interface IControllerService
{
    Task<IReadOnlyList<ControllerSlot>> GetControllersAsync(CancellationToken cancellationToken);
    Task<byte[]> ReadSpiAsync(ControllerSlot controller, uint offset, int length, CancellationToken cancellationToken);
    Task<byte[]> ReadSpiBackupAsync(
        ControllerSlot controller,
        IProgress<double>? progress,
        CancellationToken cancellationToken);
    Task WriteColorsAsync(ControllerSlot controller, byte[] colors, CancellationToken cancellationToken);
    Task WriteSpiAsync(ControllerSlot controller, uint offset, byte[] data, CancellationToken cancellationToken);
    Task RestoreSpiBackupAsync(
        ControllerSlot controller,
        byte[] backup,
        SpiRestoreScope scope,
        IProgress<double>? progress,
        CancellationToken cancellationToken);
    Task IdentifyAsync(ControllerSlot controller, CancellationToken cancellationToken);
    Task<bool> PlayRumbleRawAsync(
        ControllerSlot controller,
        byte[] samples,
        int sampleRateMilliseconds,
        CancellationToken cancellationToken);
    Task StopRumbleAsync(ControllerSlot controller, CancellationToken cancellationToken);
    Task<InputSnapshot> ReadInputAsync(ControllerSlot controller, CancellationToken cancellationToken);
    IAsyncEnumerable<InputSnapshot> StreamInputAsync(
        ControllerSlot controller,
        CancellationToken cancellationToken);
    IAsyncEnumerable<IrFrame> StreamIrAsync(
        ControllerSlot controller,
        IrCameraOptions options,
        IProgress<double>? progress,
        CancellationToken cancellationToken);
    Task<NfcTagInfo> ScanNfcAsync(
        ControllerSlot controller,
        CancellationToken cancellationToken);
    Task<DiagnosticReply> SendDiagnosticAsync(
        ControllerSlot controller,
        bool internalCommand,
        byte[] arguments,
        CancellationToken cancellationToken);
}
