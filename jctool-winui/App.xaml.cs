using JcTool.WinUI.Services;
using JcTool.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using System.Buffers.Binary;

namespace JcTool.WinUI;

public partial class App : Application
{
    private Window? _window;
    internal static Window? MainWindowInstance { get; private set; }

    public App()
    {
        var screenshotArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--screenshot=", StringComparison.OrdinalIgnoreCase));
        if (screenshotArgument is not null)
        {
            var diagnosticPath = screenshotArgument[13..].Trim('"') + ".error.log";
            UnhandledException += (_, eventArgs) =>
            {
                try
                {
                    File.WriteAllText(diagnosticPath, eventArgs.Exception?.ToString() ?? eventArgs.Message);
                }
                catch
                {
                }
            };
        }
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var languagePreference = new LanguagePreferenceService();
        var localization = new LocalizationService(languagePreference.EffectivePreference);
        Localized.Initialize(localization);
        var controllerService = new NativeControllerService(localization);
        if (Environment.GetCommandLineArgs().Any(argument =>
            argument.Equals("--operations-self-test", StringComparison.OrdinalIgnoreCase)))
        {
            var devices = controllerService
                .GetControllersAsync(CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            var controller = devices.FirstOrDefault(device => device.IsConnected);
            if (controller is null)
            {
                Environment.Exit(51);
                return;
            }

            var colors = controllerService
                .ReadSpiAsync(controller, 0x6050, 12, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            controllerService
                .WriteColorsAsync(controller, colors, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            var calibration = controllerService
                .ReadSpiAsync(controller, 0x8010, 22, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            controllerService
                .WriteSpiAsync(controller, 0x8010, calibration, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            controllerService
                .IdentifyAsync(controller, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            controllerService
                .PlayRumbleRawAsync(
                    controller,
                    new byte[] { 0x00, 0x01, 0x40, 0x40 },
                    1,
                    CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            controllerService
                .ReadInputAsync(controller, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            if (!Task.Run(() => ValidateInputStreamAsync(controllerService, controller))
                .GetAwaiter()
                .GetResult())
            {
                Environment.Exit(52);
                return;
            }
            controllerService
                .ReadInputAsync(controller, CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            var mcuController = devices.FirstOrDefault(device => device.ProductId == 0x2007);
            if (mcuController is null
                || !Task.Run(() => ValidateMcuAsync(controllerService, mcuController))
                    .GetAwaiter()
                    .GetResult())
            {
                Environment.Exit(53);
                return;
            }
            if (!Task.Run(() => ValidateSpiAndRumbleAsync(controllerService, mcuController))
                .GetAwaiter()
                .GetResult())
            {
                Environment.Exit(54);
                return;
            }
            if (!Task.Run(() => ValidateDiagnosticsAsync(controllerService, mcuController))
                .GetAwaiter()
                .GetResult())
            {
                Environment.Exit(55);
                return;
            }
            Environment.Exit(0);
            return;
        }

        if (Environment.GetCommandLineArgs().Any(argument =>
            argument.Equals("--hardware-preflight", StringComparison.OrdinalIgnoreCase)))
        {
            var devices = controllerService
                .GetControllersAsync(CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            var controller = devices.FirstOrDefault(device => device.IsConnected);
            if (controller is null)
            {
                Environment.Exit(60);
                return;
            }
            var result = Task.Run(() => HardwarePreflightRunner.RunAsync(
                    controllerService,
                    controller,
                    progress: null,
                    CancellationToken.None))
                .GetAwaiter()
                .GetResult();
            Environment.Exit(result.ExitCode);
            return;
        }

        var selfTestArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--self-test=", StringComparison.OrdinalIgnoreCase));
        if (selfTestArgument is not null
            && int.TryParse(selfTestArgument[12..], out var expectedCount))
        {
            var devices = controllerService
                .GetControllersAsync(CancellationToken.None)
                .GetAwaiter()
                .GetResult();
            Environment.Exit(devices.Count(device => device.IsConnected) == expectedCount ? 0 : 50);
            return;
        }

        var viewModel = new MainViewModel(controllerService, localization);
        _window = new MainWindow(viewModel, new WindowPlacementService(), languagePreference);
        MainWindowInstance = _window;
        _window.Activate();
    }

    private static async Task<bool> ValidateInputStreamAsync(
        IControllerService controllerService,
        Models.ControllerSlot controller)
    {
        var sampleCount = 0;
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        try
        {
            await foreach (var _ in controllerService.StreamInputAsync(controller, cancellation.Token))
            {
                if (++sampleCount == 3)
                {
                    cancellation.Cancel();
                }
            }
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
        }
        return sampleCount == 3;
    }

    private static async Task<bool> ValidateMcuAsync(
        IControllerService controllerService,
        Models.ControllerSlot controller)
    {
        using (var cancellation = new CancellationTokenSource())
        {
            var cancelProgress = new SelfTestProgress(_ => cancellation.Cancel());
            try
            {
                await foreach (var _ in controllerService.StreamIrAsync(
                    controller,
                    new Models.IrCameraOptions { Resolution = 0 },
                    cancelProgress,
                    cancellation.Token))
                {
                }
                return false;
            }
            catch (OperationCanceledException)
            {
            }
        }

        for (var resolution = 0; resolution < 4; resolution++)
        {
            Models.IrFrame? irFrame = null;
            var options = new Models.IrCameraOptions { Resolution = resolution };
            var progress = new SelfTestProgress();
            await foreach (var frame in controllerService.StreamIrAsync(
                controller,
                options,
                progress,
                CancellationToken.None))
            {
                irFrame = frame;
                break;
            }
            if (irFrame is null || irFrame.Pixels.Length != options.Width * options.Height
                || irFrame.Width != options.Width || irFrame.Height != options.Height
                || irFrame.FrameNumber == 0 || progress.Value <= 0)
            {
                return false;
            }
        }

        var tag = await controllerService.ScanNfcAsync(controller, CancellationToken.None);
        if (tag.Uid.Length != 7 || tag.TagModel != 213
            || !NdefParser.TryParse(tag.Data, out var kind, out var content)
            || kind != "Text" || content != "Simulated Joy-Con NFC tag")
        {
            return false;
        }

        using var nfcCancellation = new CancellationTokenSource(30);
        try
        {
            await controllerService.ScanNfcAsync(controller, nfcCancellation.Token);
            return false;
        }
        catch (OperationCanceledException)
        {
            return true;
        }
    }

    private static async Task<bool> ValidateSpiAndRumbleAsync(
        IControllerService controllerService,
        Models.ControllerSlot controller)
    {
        var progress = new SelfTestProgress();
        var backup = await controllerService.ReadSpiBackupAsync(
            controller,
            progress,
            CancellationToken.None);
        var hash = SpiBackupValidator.ComputeSha256(backup);
        var validation = SpiBackupValidator.Validate(backup, controller, hash);
        var wrongIdentity = backup.ToArray();
        wrongIdentity[0x15] ^= 0xff;
        var wrongProduct = backup.ToArray();
        wrongProduct[0x6012] = wrongProduct[0x6012] == 1 ? (byte)2 : (byte)1;
        if (!validation.IsValid || progress.Value < 100
            || SpiBackupValidator.Validate(wrongIdentity, controller, hash).IsValid
            || SpiBackupValidator.Validate(wrongProduct, controller, hash).IsValid)
        {
            return false;
        }

        await controllerService.RestoreSpiBackupAsync(
            controller,
            backup,
            Models.SpiRestoreScope.Colors,
            new SelfTestProgress(),
            CancellationToken.None);

        var raw = new byte[]
        {
            (byte)'R', (byte)'R', (byte)'A', (byte)'W', 0x00, 0x01,
            0x00, 0x00, 0x00, 0x02,
            0x00, 0x40, 0x40, 0x40, 0x10, 0x60, 0x60, 0x60
        };
        if (!RumbleFileParser.TryParse(raw, out var rawAsset, out _)
            || rawAsset is null || rawAsset.SourceSampleCount != 2)
        {
            return false;
        }

        var converted = RumbleFileParser.ConvertBinary(
            new byte[] { 0xff, 0xbf, 0xff, 0xdf },
            lowAmplitudePercent: 50,
            lowFrequencyPercent: 50,
            highAmplitudePercent: 50,
            highFrequencyPercent: 50);
        if (converted[0] == 0 || converted[2] == 0)
        {
            return false;
        }

        var binary = CreateBinaryRumble(0x04, 0, 0, 0);
        var loop = CreateBinaryRumble(0x0c, 1, 3, 0);
        var loopWait = CreateBinaryRumble(0x10, 1, 3, 2);
        foreach (var candidate in new[] { binary, loop, loopWait })
        {
            if (!RumbleFileParser.TryParse(candidate, out var asset, out _)
                || asset is null || asset.SourceSamples is null
                || asset.Samples.Length != asset.SourceSamples.Length
                || asset.Expand(1).Length != asset.TotalSampleCount(1) * 4)
            {
                return false;
            }
        }
        return true;
    }

    private static async Task<bool> ValidateDiagnosticsAsync(
        IControllerService controllerService,
        Models.ControllerSlot controller)
    {
        var safe = new byte[44];
        safe[0] = 0x01;
        safe[1] = 0x00;
        safe[2] = 0x01;
        safe[3] = 0x40;
        safe[4] = 0x40;
        safe[5] = 0x02;
        var safeReply = await controllerService.SendDiagnosticAsync(
            controller,
            internalCommand: false,
            arguments: safe,
            cancellationToken: CancellationToken.None);
        if (!safeReply.Matched || !safeReply.Accepted)
        {
            return false;
        }

        var unsafeCommand = safe.ToArray();
        unsafeCommand[5] = 0x11;
        try
        {
            await controllerService.SendDiagnosticAsync(
                controller,
                internalCommand: false,
                arguments: unsafeCommand,
                cancellationToken: CancellationToken.None);
            return false;
        }
        catch (InvalidOperationException)
        {
        }

        foreach (var command in new[]
        {
            new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x07 },
            new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01 },
            new byte[] { 0x01, 0x00, 0x00, 0x00, 0x00, 0x06, 0x02 }
        })
        {
            var internalPayload = new byte[44];
            command.CopyTo(internalPayload, 0);
            var internalReply = await controllerService.SendDiagnosticAsync(
                controller,
                internalCommand: true,
                arguments: internalPayload,
                cancellationToken: CancellationToken.None);
            if (!internalReply.Matched || !internalReply.Accepted)
            {
                return false;
            }
        }
        return true;
    }

    private static byte[] CreateBinaryRumble(
        byte format,
        int loopStart,
        int loopEnd,
        int loopWait)
    {
        var headerSize = format switch { 0x04 => 0x0c, 0x0c => 0x14, _ => 0x18 };
        var sampleBytes = new byte[]
        {
            0x60, 0x80, 0x70, 0x90,
            0x70, 0x90, 0x80, 0xa0,
            0x80, 0xa0, 0x90, 0xb0,
            0x90, 0xb0, 0xa0, 0xc0
        };
        var output = new byte[headerSize + sampleBytes.Length];
        output[0] = format;
        output[4] = 0x03;
        BinaryPrimitives.WriteUInt16LittleEndian(output.AsSpan(6, 2), 100);
        if (headerSize >= 0x14)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(8, 4), (uint)loopStart);
            BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(12, 4), (uint)loopEnd);
        }
        if (headerSize >= 0x18)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(output.AsSpan(16, 4), (uint)loopWait);
        }
        BinaryPrimitives.WriteUInt32LittleEndian(
            output.AsSpan(headerSize - 4, 4), (uint)sampleBytes.Length);
        sampleBytes.CopyTo(output, headerSize);
        return output;
    }

    private sealed class SelfTestProgress : IProgress<double>
    {
        private readonly Action<double>? _onReport;

        public SelfTestProgress(Action<double>? onReport = null)
        {
            _onReport = onReport;
        }

        public double Value { get; private set; }

        public void Report(double value)
        {
            Value = value;
            _onReport?.Invoke(value);
        }
    }
}
