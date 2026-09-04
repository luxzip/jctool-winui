using JcTool.WinUI.Models;

namespace JcTool.WinUI.Services;

public static class HardwarePreflightRunner
{
    public static async Task<HardwarePreflightResult> RunAsync(
        IControllerService controllerService,
        ControllerSlot controller,
        IProgress<double>? progress,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(controllerService);
        ArgumentNullException.ThrowIfNull(controller);
        progress?.Report(0);

        try
        {
            await controllerService.ReadInputAsync(controller, cancellationToken);
        }
        catch (OperationCanceledException) { throw; }
        catch { return Fail(61, "HardwarePreflightInputStep"); }
        progress?.Report(20);

        try
        {
            await controllerService.ReadSpiAsync(controller, 0x6050, 12, cancellationToken);
        }
        catch (OperationCanceledException) { throw; }
        catch { return Fail(62, "HardwarePreflightSpiStep"); }
        progress?.Report(40);

        var safe = new byte[44];
        safe[0] = 0x01;
        safe[1] = 0x00;
        safe[2] = 0x01;
        safe[3] = 0x40;
        safe[4] = 0x40;
        safe[5] = 0x02;
        try
        {
            var reply = await controllerService.SendDiagnosticAsync(
                controller,
                internalCommand: false,
                arguments: safe,
                cancellationToken: cancellationToken);
            if (!reply.Matched || !reply.Accepted)
            {
                return Fail(63, "HardwarePreflightDiagnosticStep");
            }
        }
        catch (OperationCanceledException) { throw; }
        catch { return Fail(63, "HardwarePreflightDiagnosticStep"); }

        var unsafeCommand = safe.ToArray();
        unsafeCommand[5] = 0x11;
        try
        {
            await controllerService.SendDiagnosticAsync(
                controller,
                internalCommand: false,
                arguments: unsafeCommand,
                cancellationToken: cancellationToken);
            return Fail(64, "HardwarePreflightAllowlistStep");
        }
        catch (OperationCanceledException) { throw; }
        catch (InvalidOperationException)
        {
        }
        progress?.Report(controller.ProductId == 0x2007 ? 60 : 100);

        if (controller.ProductId != 0x2007)
        {
            return Pass();
        }

        try
        {
            await foreach (var _ in controllerService.StreamIrAsync(
                controller,
                new IrCameraOptions { Resolution = 0 },
                progress: null,
                cancellationToken))
            {
                break;
            }
        }
        catch (OperationCanceledException) { throw; }
        catch { return Fail(65, "HardwarePreflightIrStep"); }
        progress?.Report(80);

        using var nfcTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        nfcTimeout.CancelAfter(500);
        try
        {
            await controllerService.ScanNfcAsync(controller, nfcTimeout.Token);
            return Fail(66, "HardwarePreflightNfcStep");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Pass();
        }
        catch (OperationCanceledException) { throw; }
        catch { return Fail(66, "HardwarePreflightNfcStep"); }
    }

    private static HardwarePreflightResult Pass() => new() { ExitCode = 0, StepResource = "HardwarePreflightPassed" };

    private static HardwarePreflightResult Fail(int code, string stepResource) => new()
    {
        ExitCode = code,
        StepResource = stepResource
    };
}
