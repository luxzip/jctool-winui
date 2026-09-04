using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using JcTool.WinUI.Models;

namespace JcTool.WinUI.Services;

public sealed class NativeControllerService : IControllerService
{
    private const int MaximumDevices = 4;
    private readonly Dictionary<string, int> _slotByDeviceKey = new(StringComparer.OrdinalIgnoreCase);
    private readonly ILocalizationService _localization;

    public NativeControllerService(ILocalizationService localization)
    {
        _localization = localization;
    }

    public Task<IReadOnlyList<ControllerSlot>> GetControllersAsync(CancellationToken cancellationToken)
    {
        return Task.Run<IReadOnlyList<ControllerSlot>>(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var devices = new NativeDevice[MaximumDevices];
            int count;

            try
            {
                count = NativeMethods.GetDevices(devices, devices.Length);
            }
            catch (DllNotFoundException)
            {
                return CreateEmptySlots(_localization.Get("BridgeUnavailable"));
            }
            catch (BadImageFormatException)
            {
                return CreateEmptySlots(_localization.Get("BridgeArchitectureMismatch"));
            }
            catch (EntryPointNotFoundException)
            {
                return CreateEmptySlots(_localization.Get("BridgeIncompatible"));
            }

            if (count < 0)
            {
                return CreateEmptySlots(_localization.Get("ControllerScanFailed"));
            }

            var result = CreateEmptySlots(_localization.Get("Disconnected")).ToArray();
            var occupiedSlots = new bool[MaximumDevices];
            var assignments = new List<(NativeDevice Device, string Key, int Slot)>();
            foreach (var source in devices.Take(Math.Min(count, devices.Length)))
            {
                var key = string.IsNullOrWhiteSpace(source.DeviceKey)
                    ? $"{source.ProductId:X4}:{source.SerialNumber}"
                    : source.DeviceKey;
                if (_slotByDeviceKey.TryGetValue(key, out var knownSlot) && !occupiedSlots[knownSlot])
                {
                    assignments.Add((source, key, knownSlot));
                    occupiedSlots[knownSlot] = true;
                }
                else
                {
                    assignments.Add((source, key, -1));
                }
            }

            for (var index = 0; index < assignments.Count; index++)
            {
                var assignment = assignments[index];
                var slot = assignment.Slot;
                if (slot < 0)
                {
                    slot = Array.FindIndex(occupiedSlots, occupied => !occupied);
                    if (slot < 0)
                    {
                        break;
                    }

                    occupiedSlots[slot] = true;
                    _slotByDeviceKey[assignment.Key] = slot;
                }

                var source = assignment.Device;
                result[slot].DisplayName = string.IsNullOrWhiteSpace(source.ProductName)
                    ? ProductName(source.ProductId)
                    : source.ProductName;
                result[slot].SerialNumber = string.IsNullOrWhiteSpace(source.SerialNumber)
                    ? _localization.Get("SerialUnavailable")
                    : source.SerialNumber;
                result[slot].StatusText = _localization.Get("Connected");
                result[slot].IsConnected = true;
                result[slot].DeviceKey = source.DeviceKey;
                result[slot].ProductId = source.ProductId;
                if (source.DetailsAvailable != 0)
                {
                    result[slot].Firmware = source.Firmware;
                    result[slot].MacAddress = source.MacAddress;
                }
                if (source.BatteryPercent >= 0)
                {
                    result[slot].BatteryText = _localization.Format(
                        "BatteryFormat",
                        source.BatteryPercent,
                        source.BatteryVoltage);
                }
                if (source.TemperatureAvailable != 0)
                {
                    result[slot].TemperatureText = _localization.Format(
                        "TemperatureFormat",
                        source.TemperatureCelsius);
                }
                if (source.ColorsAvailable != 0 && source.Colors is { Length: >= 12 })
                {
                    result[slot].BodyColor = ColorHex(source.Colors, 0);
                    result[slot].ButtonColor = ColorHex(source.Colors, 3);
                    result[slot].LeftGripColor = ColorHex(source.Colors, 6);
                    result[slot].RightGripColor = ColorHex(source.Colors, 9);
                }
            }

            return result;
        }, cancellationToken);
    }

    public Task<byte[]> ReadSpiAsync(
        ControllerSlot controller,
        uint offset,
        int length,
        CancellationToken cancellationToken)
    {
        if (length is <= 0 or > 0x80000 || offset > 0x7ffff || length > 0x80000 - offset)
        {
            throw new ArgumentOutOfRangeException(nameof(length));
        }

        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var output = new byte[length];
            var result = NativeMethods.ReadSpi(controller.DeviceKey, offset, output, output.Length);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
            return output;
        }, cancellationToken);
    }

    public async Task<byte[]> ReadSpiBackupAsync(
        ControllerSlot controller,
        IProgress<double>? progress,
        CancellationToken cancellationToken)
    {
        var backup = new byte[SpiBackupValidator.Size];
        const int chunkSize = 0x1000;
        for (var offset = 0; offset < backup.Length; offset += chunkSize)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var count = Math.Min(chunkSize, backup.Length - offset);
            var first = await ReadSpiAsync(controller, (uint)offset, count, cancellationToken);
            var verify = await ReadSpiAsync(controller, (uint)offset, count, cancellationToken);
            if (!first.AsSpan().SequenceEqual(verify))
            {
                throw new InvalidOperationException(_localization.Get("SpiBackupVerificationFailed"));
            }
            first.CopyTo(backup, offset);
            progress?.Report((offset + count) * 100d / backup.Length);
        }
        return backup;
    }

    public Task WriteColorsAsync(
        ControllerSlot controller,
        byte[] colors,
        CancellationToken cancellationToken)
    {
        var requiredLength = controller.ProductId == 0x2009 ? 12 : 6;
        if (colors.Length < requiredLength)
        {
            throw new ArgumentException("Insufficient color data.", nameof(colors));
        }

        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.WriteColors(controller.DeviceKey, colors, requiredLength);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
        }, cancellationToken);
    }

    public Task WriteSpiAsync(
        ControllerSlot controller,
        uint offset,
        byte[] data,
        CancellationToken cancellationToken)
    {
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.WriteSpi(controller.DeviceKey, offset, data, data.Length);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
        }, cancellationToken);
    }

    public async Task RestoreSpiBackupAsync(
        ControllerSlot controller,
        byte[] backup,
        SpiRestoreScope scope,
        IProgress<double>? progress,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(backup);
        var validation = SpiBackupValidator.Validate(backup, controller);
        if (!validation.IsValid)
        {
            throw new InvalidOperationException(_localization.Get(validation.MessageResource));
        }

        var segments = scope switch
        {
            SpiRestoreScope.Colors => new[] { (Offset: 0x6050u, Length: 12) },
            SpiRestoreScope.SerialNumber when controller.ProductId != 0x2009 => new[] { (Offset: 0x6000u, Length: 0x10) },
            SpiRestoreScope.SerialNumber => throw new InvalidOperationException(_localization.Get("SerialNumberProUnsupported")),
            SpiRestoreScope.UserCalibration => controller.ProductId switch
            {
                0x2006 => new[] { (Offset: 0x8010u, Length: 0x0b), (Offset: 0x8026u, Length: 0x1a) },
                0x2007 => new[] { (Offset: 0x801bu, Length: 0x0b), (Offset: 0x8026u, Length: 0x1a) },
                _ => new[] { (Offset: 0x8010u, Length: 0x0b), (Offset: 0x801bu, Length: 0x0b), (Offset: 0x8026u, Length: 0x1a) }
            },
            SpiRestoreScope.Full => new[] { (Offset: 0x6000u, Length: 0x1000), (Offset: 0x8000u, Length: 0x1000), (Offset: 0xf000u, Length: 0x10) },
            _ => throw new ArgumentOutOfRangeException(nameof(scope))
        };

        var total = segments.Sum(segment => segment.Length);
        var completed = 0;
        foreach (var segment in segments)
        {
            var data = scope == SpiRestoreScope.Full && segment.Offset == 0xf000u
                ? Enumerable.Repeat((byte)0xff, segment.Length).ToArray()
                : backup.AsSpan(checked((int)segment.Offset), segment.Length).ToArray();
            await WriteSpiChunksAsync(controller, segment.Offset, data, cancellationToken);
            completed += segment.Length;
            progress?.Report(completed * 100d / total);
        }
    }

    private async Task WriteSpiChunksAsync(
        ControllerSlot controller,
        uint offset,
        byte[] data,
        CancellationToken cancellationToken)
    {
        const int chunkSize = 0x1000;
        for (var position = 0; position < data.Length; position += chunkSize)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var count = Math.Min(chunkSize, data.Length - position);
            var chunk = data.AsSpan(position, count).ToArray();
            await WriteSpiAsync(controller, offset + (uint)position, chunk, cancellationToken);
        }
    }

    public Task IdentifyAsync(ControllerSlot controller, CancellationToken cancellationToken)
    {
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.Identify(controller.DeviceKey);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
        }, cancellationToken);
    }

    public Task<bool> PlayRumbleRawAsync(
        ControllerSlot controller,
        byte[] samples,
        int sampleRateMilliseconds,
        CancellationToken cancellationToken)
    {
        if (samples.Length == 0 || samples.Length % 4 != 0)
        {
            throw new ArgumentException("Rumble samples must contain complete four-byte frames.", nameof(samples));
        }

        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.PlayRumbleRaw(
                controller.DeviceKey,
                samples,
                samples.Length / 4,
                sampleRateMilliseconds);
            if (result == 4)
            {
                return false;
            }
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
            return true;
        }, cancellationToken);
    }

    public Task StopRumbleAsync(ControllerSlot controller, CancellationToken cancellationToken)
    {
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.StopRumble(controller.DeviceKey);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
        }, cancellationToken);
    }

    public Task<InputSnapshot> ReadInputAsync(
        ControllerSlot controller,
        CancellationToken cancellationToken)
    {
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = NativeMethods.ReadInput(controller.DeviceKey, out var snapshot);
            if (result != 0)
            {
                throw new InvalidOperationException(_localization.Format("NativeOperationFailedFormat", result));
            }
            return ToInputSnapshot(snapshot);
        }, cancellationToken);
    }

    public async IAsyncEnumerable<InputSnapshot> StreamInputAsync(
        ControllerSlot controller,
        [EnumeratorCancellation] CancellationToken cancellationToken)
    {
        var startResult = await Task.Run(
            () => NativeMethods.StartInputStream(controller.DeviceKey),
            cancellationToken);
        if (startResult != 0)
        {
            throw new InvalidOperationException(
                _localization.Format("NativeOperationFailedFormat", startResult));
        }

        try
        {
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var read = await Task.Run(() =>
                {
                    var result = NativeMethods.ReadInputStream(
                        controller.DeviceKey,
                        out var snapshot,
                        100);
                    return (Result: result, Snapshot: snapshot);
                });

                if (read.Result == 4)
                {
                    continue;
                }
                if (read.Result != 0)
                {
                    throw new InvalidOperationException(
                        _localization.Format("NativeOperationFailedFormat", read.Result));
                }

                yield return ToInputSnapshot(read.Snapshot);
                await Task.Delay(16, cancellationToken);
            }
        }
        finally
        {
            await Task.Run(() => NativeMethods.StopInputStream(controller.DeviceKey));
        }
    }

    public async IAsyncEnumerable<IrFrame> StreamIrAsync(
        ControllerSlot controller,
        IrCameraOptions options,
        IProgress<double>? progress,
        [EnumeratorCancellation] CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(options);
        var nativeConfig = new NativeIrConfig
        {
            Resolution = options.Resolution,
            ExposureMicroseconds = options.ExposureMicroseconds,
            DigitalGain = options.DigitalGain,
            LedsEnabled = options.LedsEnabled ? 1 : 0,
            ExternalLightFilter = options.ExternalLightFilter ? 1 : 0,
            FlipHorizontal = options.FlipHorizontal ? 1 : 0,
            Denoise = options.Denoise ? 1 : 0
        };
        var startResult = await Task.Run(
            () => NativeMethods.StartIrStream(controller.DeviceKey, ref nativeConfig),
            cancellationToken);
        if (startResult != 0)
        {
            throw new InvalidOperationException(
                _localization.Format("NativeOperationFailedFormat", startResult));
        }

        var frameBuffer = new byte[options.Width * options.Height];
        try
        {
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var read = await Task.Run(() =>
                {
                    var result = NativeMethods.ReadIrFrameFragment(
                        controller.DeviceKey,
                        frameBuffer,
                        frameBuffer.Length,
                        out var frameInfo,
                        120);
                    return (Result: result, FrameInfo: frameInfo);
                });

                if (read.Result == 4)
                {
                    continue;
                }
                if (read.Result != 0)
                {
                    throw new InvalidOperationException(
                        _localization.Format("NativeOperationFailedFormat", read.Result));
                }

                progress?.Report(read.FrameInfo.Progress);
                if (read.FrameInfo.FrameReady == 0)
                {
                    continue;
                }
                yield return new IrFrame
                {
                    Pixels = frameBuffer.ToArray(),
                    Width = read.FrameInfo.Width,
                    Height = read.FrameInfo.Height,
                    AverageIntensity = read.FrameInfo.AverageIntensity,
                    WhitePixels = read.FrameInfo.WhitePixels,
                    AmbientPixels = read.FrameInfo.AmbientPixels,
                    FrameNumber = read.FrameInfo.FrameNumber
                };
            }
        }
        finally
        {
            await Task.Run(() => NativeMethods.StopIrStream(controller.DeviceKey));
        }
    }

    public async Task<NfcTagInfo> ScanNfcAsync(
        ControllerSlot controller,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var data = new byte[924];
        using var registration = cancellationToken.Register(
            () => NativeMethods.CancelNfc(controller.DeviceKey));
        var scan = await Task.Run(() =>
            {
                var result = NativeMethods.ScanNfc(
                    controller.DeviceKey,
                    out var tag,
                    data,
                    data.Length,
                    30000);
                return (Result: result, Tag: tag);
            },
            cancellationToken);
        if (scan.Result == 4)
        {
            cancellationToken.ThrowIfCancellationRequested();
        }
        if (scan.Result != 0)
        {
            throw new InvalidOperationException(
                _localization.Format("NativeOperationFailedFormat", scan.Result));
        }
        var tag = scan.Tag;
        return new NfcTagInfo
        {
            Uid = tag.Uid.Take(Math.Clamp(tag.UidLength, 0, tag.Uid.Length)).ToArray(),
            TagType = tag.TagType,
            TagModel = tag.TagModel,
            Data = data.Take(Math.Clamp(tag.DataLength, 0, data.Length)).ToArray()
        };
    }

    public async Task<DiagnosticReply> SendDiagnosticAsync(
        ControllerSlot controller,
        bool internalCommand,
        byte[] arguments,
        CancellationToken cancellationToken)
    {
        if (arguments is null || arguments.Length < 6 || arguments.Length > 44)
        {
            throw new ArgumentException("Diagnostic command must contain 6 to 44 bytes.", nameof(arguments));
        }
        cancellationToken.ThrowIfCancellationRequested();
        var reply = await Task.Run(() =>
        {
            var nativeReply = new NativeDiagnosticReply { Data = new byte[0x170] };
            var result = NativeMethods.SendDiagnostic(
                controller.DeviceKey,
                internalCommand ? 1 : 0,
                arguments,
                arguments.Length,
                ref nativeReply);
            return (Result: result, Reply: nativeReply);
        }, cancellationToken);
        if (reply.Result != 0)
        {
            throw new InvalidOperationException(
                _localization.Format("DiagnosticOperationFailedFormat", reply.Result));
        }
        return new DiagnosticReply
        {
            Sent = arguments.ToArray(),
            Received = reply.Reply.Data.Take(Math.Clamp(reply.Reply.Length, 0, reply.Reply.Data.Length)).ToArray(),
            Matched = reply.Reply.Matched != 0,
            Accepted = reply.Reply.Accepted != 0
        };
    }

    private IReadOnlyList<ControllerSlot> CreateEmptySlots(string status)
    {
        return Enumerable.Range(0, MaximumDevices)
            .Select(index => new ControllerSlot(index, _localization) { StatusText = status })
            .ToArray();
    }

    private string ProductName(uint productId) => productId switch
    {
        0x2006 => "Joy-Con (L)",
        0x2007 => "Joy-Con (R)",
        0x2009 => "Pro Controller",
        _ => _localization.Get("NintendoController")
    };

    private static string ColorHex(byte[] colors, int offset)
    {
        return $"#{colors[offset]:X2}{colors[offset + 1]:X2}{colors[offset + 2]:X2}";
    }

    private static InputSnapshot ToInputSnapshot(NativeInputSnapshot snapshot)
    {
        return new InputSnapshot
        {
            Buttons = snapshot.Buttons,
            LeftX = snapshot.LeftX,
            LeftY = snapshot.LeftY,
            RightX = snapshot.RightX,
            RightY = snapshot.RightY,
            AccelerationX = snapshot.AccelerationX,
            AccelerationY = snapshot.AccelerationY,
            AccelerationZ = snapshot.AccelerationZ,
            GyroscopeX = snapshot.GyroscopeX,
            GyroscopeY = snapshot.GyroscopeY,
            GyroscopeZ = snapshot.GyroscopeZ,
            ConnectionType = snapshot.ConnectionType,
            BatteryLevel = snapshot.BatteryLevel,
            Charging = snapshot.Charging != 0
        };
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeDevice
    {
        public uint ProductId;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string SerialNumber;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 96)]
        public string ProductName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 192)]
        public string DeviceKey;

        public int DetailsAvailable;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]
        public string Firmware;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string MacAddress;

        public int BatteryPercent;
        public int BatteryCharging;
        public float BatteryVoltage;
        public float TemperatureCelsius;
        public int TemperatureAvailable;
        public int ColorsAvailable;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12, ArraySubType = UnmanagedType.U1)]
        public byte[] Colors;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeInputSnapshot
    {
        public uint Buttons;
        public int LeftX;
        public int LeftY;
        public int RightX;
        public int RightY;
        public short AccelerationX;
        public short AccelerationY;
        public short AccelerationZ;
        public short GyroscopeX;
        public short GyroscopeY;
        public short GyroscopeZ;
        public int ConnectionType;
        public int BatteryLevel;
        public int Charging;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeIrConfig
    {
        public int Resolution;
        public int ExposureMicroseconds;
        public int DigitalGain;
        public int LedsEnabled;
        public int ExternalLightFilter;
        public int FlipHorizontal;
        public int Denoise;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeIrFrameInfo
    {
        public int Width;
        public int Height;
        public int Progress;
        public int FrameReady;
        public int AverageIntensity;
        public int WhitePixels;
        public int AmbientPixels;
        public uint FrameNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeNfcTag
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10, ArraySubType = UnmanagedType.U1)]
        public byte[] Uid;
        public int UidLength;
        public int TagType;
        public int TagModel;
        public int DataLength;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeDiagnosticReply
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 0x170, ArraySubType = UnmanagedType.U1)]
        public byte[] Data;
        public int Length;
        public int Matched;
        public int Accepted;
    }

    private static class NativeMethods
    {
        [DllImport("JcTool.Native.dll", EntryPoint = "jc_get_devices",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int GetDevices([Out] NativeDevice[] devices, int capacity);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_read_spi",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int ReadSpi(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            uint offset,
            [Out] byte[] output,
            int length);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_write_colors",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int WriteColors(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            byte[] colors,
            int length);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_write_spi",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int WriteSpi(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            uint offset,
            byte[] data,
            int length);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_identify",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int Identify([MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_play_rumble_raw",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int PlayRumbleRaw(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            byte[] samples,
            int sampleCount,
            int sampleRateMilliseconds);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_stop_rumble",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int StopRumble([MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_read_input",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int ReadInput(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            out NativeInputSnapshot snapshot);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_start_input_stream",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int StartInputStream(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_read_input_stream",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int ReadInputStream(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            out NativeInputSnapshot snapshot,
            int timeoutMilliseconds);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_stop_input_stream",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int StopInputStream(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_start_ir_stream",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int StartIrStream(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            ref NativeIrConfig config);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_read_ir_frame_fragment",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int ReadIrFrameFragment(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            [Out] byte[] frame,
            int frameCapacity,
            out NativeIrFrameInfo frameInfo,
            int timeoutMilliseconds);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_stop_ir_stream",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int StopIrStream(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_scan_nfc",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int ScanNfc(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            out NativeNfcTag tag,
            [Out] byte[] tagData,
            int tagDataCapacity,
            int timeoutMilliseconds);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_cancel_nfc",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int CancelNfc(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey);

        [DllImport("JcTool.Native.dll", EntryPoint = "jc_send_diagnostic",
            CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern int SendDiagnostic(
            [MarshalAs(UnmanagedType.LPWStr)] string deviceKey,
            int internalCommand,
            byte[] arguments,
            int argumentLength,
            ref NativeDiagnosticReply reply);
    }
}
