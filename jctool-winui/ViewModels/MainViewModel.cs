using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using JcTool.WinUI.Models;
using JcTool.WinUI.Services;

namespace JcTool.WinUI.ViewModels;

public sealed class MainViewModel : INotifyPropertyChanged
{
    private readonly IControllerService _controllerService;
    private readonly ILocalizationService _localization;
    private ControllerSlot? _selectedController;
    private bool _isRefreshing;
    private bool _isOperating;
    private bool _isRumblePlaying;
    private bool _isInputStreaming;
    private bool _isIrStreaming;
    private bool _isNfcScanning;
    private ControllerSlot? _rumbleController;
    private CancellationTokenSource? _activeOperationCancellation;
    private Func<Task>? _activeCancelAction;
    private double _operationProgress;
    private bool _isOperationIndeterminate;
    private bool _canCancelOperation;
    private string _statusText;

    public MainViewModel(IControllerService controllerService, ILocalizationService localization)
    {
        _controllerService = controllerService;
        _localization = localization;
        _statusText = localization.Get("Ready");
        Devices = new ObservableCollection<ControllerSlot>(
            Enumerable.Range(0, 4).Select(index => new ControllerSlot(index, localization)));
    }

    public ObservableCollection<ControllerSlot> Devices { get; }

    public ControllerSlot? SelectedController
    {
        get => _selectedController;
        private set
        {
            if (ReferenceEquals(_selectedController, value))
            {
                return;
            }

            if (_selectedController is not null)
            {
                _selectedController.IsSelected = false;
            }

            _selectedController = value;
            if (_selectedController is not null)
            {
                _selectedController.IsSelected = true;
            }

            OnPropertyChanged();
            OnPropertyChanged(nameof(SelectedControllerName));
            OnPropertyChanged(nameof(SelectedControllerSerial));
            OnPropertyChanged(nameof(SelectedFirmware));
            OnPropertyChanged(nameof(SelectedMacAddress));
            OnPropertyChanged(nameof(SelectedBattery));
            OnPropertyChanged(nameof(SelectedTemperature));
            OnPropertyChanged(nameof(SelectedSupportsIr));
            OnPropertyChanged(nameof(SelectedSupportsNfc));
            OnPropertyChanged(nameof(HasSelectedController));
            OnPropertyChanged(nameof(SelectedSupportsIr));
            OnPropertyChanged(nameof(SelectedSupportsNfc));
        }
    }

    public string SelectedControllerName => SelectedController?.DisplayName ?? _localization.Get("NoControllerSelected");
    public string SelectedControllerSerial => SelectedController?.SerialNumber ?? _localization.Get("ConnectThenRefresh");
    public string SelectedFirmware => SelectedController?.Firmware ?? _localization.Get("NotReadValue");
    public string SelectedMacAddress => SelectedController?.MacAddress ?? _localization.Get("NotReadValue");
    public string SelectedBattery => SelectedController?.BatteryText ?? _localization.Get("NotReadValue");
    public string SelectedTemperature => SelectedController?.TemperatureText ?? _localization.Get("NotReadValue");
    public bool HasSelectedController => SelectedController?.IsConnected == true;
    public bool SelectedSupportsIr => SelectedController is { IsConnected: true, ProductId: 0x2007 };
    public bool SelectedSupportsNfc => SelectedController is { IsConnected: true, ProductId: 0x2007 or 0x2009 };
    public string CancelOperationText => _localization.Get("CancelOperation");

    public bool IsRefreshing
    {
        get => _isRefreshing;
        private set
        {
            SetField(ref _isRefreshing, value);
            OnPropertyChanged(nameof(IsBusy));
            OnPropertyChanged(nameof(IsBusyIndeterminate));
            OnPropertyChanged(nameof(CanStartOperation));
        }
    }

    public bool IsOperating
    {
        get => _isOperating;
        private set
        {
            SetField(ref _isOperating, value);
            OnPropertyChanged(nameof(IsBusy));
            OnPropertyChanged(nameof(IsBusyIndeterminate));
            OnPropertyChanged(nameof(IsOperationProgressVisible));
            OnPropertyChanged(nameof(CanStartOperation));
        }
    }

    public bool IsBusy => IsRefreshing || IsOperating;
    public bool CanStartOperation => !IsBusy;
    public bool IsBusyIndeterminate => IsRefreshing || IsOperating && IsOperationIndeterminate;
    public bool IsOperationProgressVisible => IsOperating && !IsOperationIndeterminate;

    public double OperationProgress
    {
        get => _operationProgress;
        private set => SetField(ref _operationProgress, Math.Clamp(value, 0, 100));
    }

    public bool IsOperationIndeterminate
    {
        get => _isOperationIndeterminate;
        private set
        {
            SetField(ref _isOperationIndeterminate, value);
            OnPropertyChanged(nameof(IsBusyIndeterminate));
            OnPropertyChanged(nameof(IsOperationProgressVisible));
        }
    }

    public bool CanCancelOperation
    {
        get => _canCancelOperation;
        private set => SetField(ref _canCancelOperation, value);
    }

    public bool IsRumblePlaying
    {
        get => _isRumblePlaying;
        private set => SetField(ref _isRumblePlaying, value);
    }

    public bool IsInputStreaming
    {
        get => _isInputStreaming;
        private set => SetField(ref _isInputStreaming, value);
    }

    public bool IsIrStreaming
    {
        get => _isIrStreaming;
        private set => SetField(ref _isIrStreaming, value);
    }

    public bool IsNfcScanning
    {
        get => _isNfcScanning;
        private set => SetField(ref _isNfcScanning, value);
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetField(ref _statusText, value);
    }

    public async Task RefreshAsync(CancellationToken cancellationToken = default)
    {
        if (IsRefreshing || IsOperating)
        {
            return;
        }

        IsRefreshing = true;
        StatusText = _localization.Get("ScanningControllers");
        try
        {
            var refreshed = await _controllerService.GetControllersAsync(cancellationToken);
            var preferredSerial = SelectedController?.SerialNumber;
            for (var index = 0; index < Devices.Count; index++)
            {
                var source = refreshed[index];
                var target = Devices[index];
                target.DisplayName = source.DisplayName;
                target.SerialNumber = source.SerialNumber;
                target.StatusText = source.StatusText;
                target.IsConnected = source.IsConnected;
                target.DeviceKey = source.DeviceKey;
                target.ProductId = source.ProductId;
                target.Firmware = source.Firmware;
                target.MacAddress = source.MacAddress;
                target.BatteryText = source.BatteryText;
                target.TemperatureText = source.TemperatureText;
                target.BodyColor = source.BodyColor;
                target.ButtonColor = source.ButtonColor;
                target.LeftGripColor = source.LeftGripColor;
                target.RightGripColor = source.RightGripColor;
            }

            SelectedController = Devices.FirstOrDefault(device =>
                device.IsConnected && device.SerialNumber == preferredSerial)
                ?? Devices.FirstOrDefault(device => device.IsConnected);
            OnPropertyChanged(nameof(SelectedFirmware));
            OnPropertyChanged(nameof(SelectedMacAddress));
            OnPropertyChanged(nameof(SelectedBattery));
            OnPropertyChanged(nameof(SelectedTemperature));

            var connectedCount = Devices.Count(device => device.IsConnected);
            StatusText = connectedCount == 0
                ? _localization.Get("NoControllersDetected")
                : connectedCount == 1
                    ? _localization.Get("OneControllerConnected")
                    : _localization.Format("ControllersConnectedFormat", connectedCount);
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("ControllerScanCancelled");
        }
        finally
        {
            IsRefreshing = false;
        }
    }

    public void SelectController(int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= Devices.Count || !Devices[slotIndex].IsConnected)
        {
            return;
        }

        if (IsOperating)
        {
            _ = CancelActiveOperationAsync();
        }

        SelectedController = Devices[slotIndex];
        StatusText = _localization.Format("SelectedControllerFormat", SelectedController.DisplayName);
    }

    public async Task<bool> IdentifySelectedAsync(CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }

        return await RunOperationAsync(
            token => _controllerService.IdentifyAsync(controller, token),
            "IdentifySucceeded",
            cancellationToken);
    }

    public async Task<bool> WriteSelectedColorsAsync(byte[] colors, CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }

        var succeeded = await RunOperationAsync(
            token => _controllerService.WriteColorsAsync(controller, colors, token),
            "ColorsWritten",
            cancellationToken);
        if (succeeded)
        {
            await RefreshAsync(cancellationToken);
            StatusText = _localization.Get("ColorsWritten");
        }
        return succeeded;
    }

    public async Task<byte[]?> ReadSelectedSpiAsync(
        uint offset,
        int length,
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }

        var operation = BeginOperation("ReadingSpi", cancellationToken);
        if (operation is null)
        {
            return null;
        }
        try
        {
            var result = await _controllerService.ReadSpiAsync(controller, offset, length, operation.Token);
            StatusText = _localization.Format("SpiReadSucceededFormat", result.Length);
            return result;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return null;
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    public async Task<bool> WriteSelectedSpiAsync(
        uint offset,
        byte[] data,
        string successResource,
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }

        return await RunOperationAsync(
            token => _controllerService.WriteSpiAsync(controller, offset, data, token),
            successResource,
            cancellationToken);
    }

    public async Task PlayRumbleAsync(
        byte[] samples,
        int sampleRateMilliseconds,
        CancellationToken cancellationToken = default)
    {
        if (SelectedController is null || IsRumblePlaying)
        {
            StatusText = SelectedController is null
                ? _localization.Get("SelectControllerFirst")
                : _localization.Get("ControllerBusy");
            return;
        }

        var rumbleController = SelectedController;
        _rumbleController = rumbleController;
        var operation = BeginOperation(
            "RumblePlaying",
            cancellationToken,
            canCancel: true,
            cancelAction: () => _controllerService.StopRumbleAsync(
                rumbleController,
                CancellationToken.None));
        if (operation is null)
        {
            _rumbleController = null;
            return;
        }
        IsRumblePlaying = true;
        try
        {
            var completed = await _controllerService.PlayRumbleRawAsync(
                _rumbleController,
                samples,
                sampleRateMilliseconds,
                operation.Token);
            StatusText = _localization.Get(completed ? "RumbleCompleted" : "RumbleStopped");
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("RumbleStopped");
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
        }
        finally
        {
            IsRumblePlaying = false;
            EndOperation(operation);
            _rumbleController = null;
        }
    }

    public async Task StopRumbleAsync(CancellationToken cancellationToken = default)
    {
        if (_rumbleController is null)
        {
            return;
        }
        await CancelActiveOperationAsync("RumbleStopping");
    }

    public async Task<InputSnapshot?> ReadInputAsync(CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }

        if (IsOperating)
        {
            StatusText = _localization.Get("ControllerBusy");
            return null;
        }

        var operation = BeginOperation("ReadingInput", cancellationToken);
        if (operation is null)
        {
            return null;
        }
        try
        {
            var snapshot = await _controllerService.ReadInputAsync(controller, operation.Token);
            StatusText = _localization.Get("InputReadSucceeded");
            return snapshot;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return null;
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    public async Task StreamInputAsync(
        Action<InputSnapshot> onSnapshot,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(onSnapshot);
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return;
        }
        if (IsOperating)
        {
            StatusText = _localization.Get("ControllerBusy");
            return;
        }

        var operation = BeginOperation(
            "InputStreamStarted",
            cancellationToken,
            canCancel: true);
        if (operation is null)
        {
            return;
        }
        IsInputStreaming = true;
        try
        {
            await foreach (var snapshot in _controllerService.StreamInputAsync(
                controller,
                operation.Token))
            {
                onSnapshot(snapshot);
            }
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested)
        {
            StatusText = _localization.Get("InputStreamStopped");
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
        }
        finally
        {
            IsInputStreaming = false;
            EndOperation(operation);
        }
    }

    public void StopInputStream()
    {
        _ = CancelActiveOperationAsync("InputStreamStopping");
    }

    public async Task CaptureIrAsync(
        IrCameraOptions options,
        Action<IrFrame> onFrame,
        bool continuous,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(onFrame);
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return;
        }
        if (!SelectedSupportsIr)
        {
            StatusText = _localization.Get("IrRequiresRightJoyCon");
            return;
        }

        var operation = BeginOperation(
            continuous ? "IrStreaming" : "IrCapturing",
            cancellationToken,
            canCancel: true,
            indeterminate: false);
        if (operation is null)
        {
            return;
        }

        IsIrStreaming = continuous;
        var progress = new Progress<double>(UpdateOperationProgress);
        try
        {
            await foreach (var frame in _controllerService.StreamIrAsync(
                controller,
                options,
                progress,
                operation.Token))
            {
                onFrame(frame);
                StatusText = continuous
                    ? _localization.Format("IrFrameReceivedFormat", frame.FrameNumber)
                    : _localization.Get("IrCaptureCompleted");
                if (!continuous)
                {
                    break;
                }
            }
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested)
        {
            StatusText = _localization.Get("IrStopped");
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
        }
        finally
        {
            IsIrStreaming = false;
            EndOperation(operation);
        }
    }

    public void StopIrStream()
    {
        _ = CancelActiveOperationAsync("IrStopping");
    }

    public async Task<NfcTagInfo?> ScanNfcAsync(CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }
        if (!SelectedSupportsNfc)
        {
            StatusText = _localization.Get("NfcRequiresRightJoyConOrPro");
            return null;
        }

        var operation = BeginOperation(
            "NfcScanning",
            cancellationToken,
            canCancel: true);
        if (operation is null)
        {
            return null;
        }
        IsNfcScanning = true;
        try
        {
            var tag = await _controllerService.ScanNfcAsync(controller, operation.Token);
            StatusText = _localization.Get("NfcTagRead");
            return tag;
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested)
        {
            StatusText = _localization.Get("NfcStopped");
            return null;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            IsNfcScanning = false;
            EndOperation(operation);
        }
    }

    public void StopNfcScan()
    {
        _ = CancelActiveOperationAsync("NfcStopping");
    }

    public async Task<DiagnosticReply?> SendDiagnosticCommandAsync(
        bool internalCommand,
        byte[] arguments,
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }
        var operation = BeginOperation(
            "DiagnosticSending",
            cancellationToken,
            canCancel: true);
        if (operation is null)
        {
            return null;
        }

        try
        {
            var reply = await _controllerService.SendDiagnosticAsync(
                controller,
                internalCommand,
                arguments,
                operation.Token);
            StatusText = reply.Accepted
                ? _localization.Get("DiagnosticAccepted")
                : _localization.Get("DiagnosticReplyReceived");
            return reply;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return null;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    public async Task<HardwarePreflightResult?> RunHardwarePreflightAsync(
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }

        var operation = BeginOperation(
            "HardwarePreflightRunning",
            cancellationToken,
            canCancel: true,
            indeterminate: false);
        if (operation is null)
        {
            return null;
        }
        try
        {
            var result = await HardwarePreflightRunner.RunAsync(
                _controllerService,
                controller,
                new Progress<double>(UpdateOperationProgress),
                operation.Token);
            StatusText = result.Succeeded
                ? _localization.Get("HardwarePreflightPassed")
                : _localization.Format(
                    "HardwarePreflightFailedFormat",
                    result.ExitCode,
                    _localization.Get(result.StepResource));
            return result;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return null;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    public async Task<bool> FinalizeFullRestoreAsync(CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }
        var operation = BeginOperation(
            "FullRestoreFinalizing",
            cancellationToken,
            canCancel: true,
            indeterminate: false);
        if (operation is null)
        {
            return false;
        }

        var commands = new[]
        {
            (Name: "FullRestoreShipment", Arguments: DiagnosticCommand(0x08, 0x01)),
            (Name: "FullRestoreClearPairing", Arguments: DiagnosticCommand(0x07)),
            (Name: "FullRestoreRebootPairing", Arguments: DiagnosticCommand(0x06, 0x02))
        };
        try
        {
            for (var index = 0; index < commands.Length; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                await _controllerService.SendDiagnosticAsync(
                    controller,
                    internalCommand: true,
                    arguments: commands[index].Arguments,
                    cancellationToken: operation.Token);
                UpdateOperationProgress((index + 1) * 100d / commands.Length);
            }
            StatusText = _localization.Get("FullRestoreFinalized");
            return true;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return false;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return false;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    private static byte[] DiagnosticCommand(byte subcommand, params byte[] payload)
    {
        var command = new byte[44];
        command[0] = 0x01;
        command[5] = subcommand;
        payload.CopyTo(command, 6);
        return command;
    }

    public SpiBackupValidation ValidateSpiBackup(byte[] backup, string? expectedSha256 = null)
    {
        var controller = SelectedController;
        return controller is null
            ? new SpiBackupValidation { MessageResource = "SelectControllerFirst" }
            : SpiBackupValidator.Validate(backup, controller, expectedSha256);
    }

    public async Task<byte[]?> CreateSpiBackupAsync(CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return null;
        }

        var operation = BeginOperation(
            "SpiBackupCreating",
            cancellationToken,
            canCancel: true,
            indeterminate: false);
        if (operation is null)
        {
            return null;
        }

        try
        {
            var progress = new Progress<double>(UpdateOperationProgress);
            var backup = await _controllerService.ReadSpiBackupAsync(
                controller,
                progress,
                operation.Token);
            StatusText = _localization.Get("SpiBackupCreated");
            return backup;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return null;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return null;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    public async Task<bool> RestoreSpiBackupAsync(
        byte[] backup,
        SpiRestoreScope scope,
        string? expectedSha256 = null,
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }

        var validation = SpiBackupValidator.Validate(backup, controller, expectedSha256);
        if (string.IsNullOrWhiteSpace(expectedSha256) || !validation.IsValid)
        {
            StatusText = _localization.Get(string.IsNullOrWhiteSpace(expectedSha256)
                ? "SpiBackupChecksumMissing"
                : validation.MessageResource);
            return false;
        }

        var operation = BeginOperation(
            "SpiRestoreWriting",
            cancellationToken,
            canCancel: true,
            indeterminate: false);
        if (operation is null)
        {
            return false;
        }

        var restored = false;
        try
        {
            var progress = new Progress<double>(UpdateOperationProgress);
            await _controllerService.RestoreSpiBackupAsync(
                controller,
                backup,
                scope,
                progress,
                operation.Token);
            StatusText = _localization.Get("SpiRestoreCompleted");
            restored = true;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
        }
        finally
        {
            EndOperation(operation);
        }
        if (restored)
        {
            await RefreshAsync(CancellationToken.None);
        }
        return restored;
    }

    public async Task<bool> WriteSerialNumberAsync(
        string serialNumber,
        CancellationToken cancellationToken = default)
    {
        var controller = SelectedController;
        if (controller is null)
        {
            StatusText = _localization.Get("SelectControllerFirst");
            return false;
        }
        if (controller.ProductId == 0x2009)
        {
            StatusText = _localization.Get("SerialNumberProUnsupported");
            return false;
        }
        if (string.IsNullOrWhiteSpace(serialNumber)
            || serialNumber.Length > 15
            || serialNumber.Any(character => character is < (char)32 or > (char)126))
        {
            StatusText = _localization.Get("SerialNumberInvalid");
            return false;
        }

        var data = new byte[0x10];
        var bytes = System.Text.Encoding.ASCII.GetBytes(serialNumber);
        bytes.CopyTo(data, data.Length - bytes.Length);
        var success = await WriteSelectedSpiAsync(
            0x6000,
            data,
            "SerialNumberWritten",
            cancellationToken);
        if (success)
        {
            await RefreshAsync(CancellationToken.None);
        }
        return success;
    }

    public async Task CancelActiveOperationAsync(string statusResource = "OperationCancelling")
    {
        var cancellation = _activeOperationCancellation;
        if (cancellation is null || cancellation.IsCancellationRequested || !CanCancelOperation)
        {
            return;
        }

        CanCancelOperation = false;
        StatusText = _localization.Get(statusResource);
        cancellation.Cancel();
        var cancelAction = _activeCancelAction;
        if (cancelAction is null)
        {
            return;
        }
        try
        {
            await cancelAction();
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
        }
    }

    public void UpdateOperationProgress(double progress)
    {
        OperationProgress = progress;
    }

    private async Task<bool> RunOperationAsync(
        Func<CancellationToken, Task> action,
        string successResource,
        CancellationToken cancellationToken)
    {
        var operation = BeginOperation("Working", cancellationToken);
        if (operation is null)
        {
            return false;
        }

        try
        {
            await action(operation.Token);
            StatusText = _localization.Get(successResource);
            return true;
        }
        catch (OperationCanceledException)
        {
            StatusText = _localization.Get("OperationCancelled");
            return false;
        }
        catch (Exception exception)
        {
            StatusText = _localization.Format("OperationFailedFormat", exception.Message);
            return false;
        }
        finally
        {
            EndOperation(operation);
        }
    }

    private CancellationTokenSource? BeginOperation(
        string statusResource,
        CancellationToken cancellationToken,
        bool canCancel = false,
        bool indeterminate = true,
        Func<Task>? cancelAction = null)
    {
        if (IsOperating)
        {
            StatusText = _localization.Get("ControllerBusy");
            return null;
        }

        var operation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _activeOperationCancellation = operation;
        _activeCancelAction = cancelAction;
        OperationProgress = 0;
        IsOperationIndeterminate = indeterminate;
        CanCancelOperation = canCancel;
        IsOperating = true;
        StatusText = _localization.Get(statusResource);
        return operation;
    }

    private void EndOperation(CancellationTokenSource operation)
    {
        if (!ReferenceEquals(_activeOperationCancellation, operation))
        {
            operation.Dispose();
            return;
        }

        _activeOperationCancellation = null;
        _activeCancelAction = null;
        CanCancelOperation = false;
        IsOperationIndeterminate = false;
        OperationProgress = 0;
        IsOperating = false;
        operation.Dispose();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Text(string key) => _localization.Get(key);

    private void SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }

        field = value;
        OnPropertyChanged(propertyName);
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
