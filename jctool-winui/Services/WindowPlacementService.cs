using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.Graphics;

namespace JcTool.WinUI.Services;

public sealed class WindowPlacementService
{
    private const int DefaultWidth = 1180;
    private const int DefaultHeight = 760;
    private const int MinimumWidth = 760;
    private const int MinimumHeight = 560;
    private readonly string _settingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "JcTool",
        "ui-settings.json");

    private nint _windowHandle;
    private AppWindow? _appWindow;
    private RectInt32 _lastNormalBounds;
    private uint _lastNormalDpi = 96;
    private bool _enforcingMinimumSize;
    private bool _hasSaved;

    public void Attach(Window window)
    {
        _windowHandle = WinRT.Interop.WindowNative.GetWindowHandle(window);
        var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_windowHandle);
        _appWindow = AppWindow.GetFromWindowId(windowId);
        Restore();
        CaptureNormalBounds();
        _appWindow.Changed += OnWindowChanged;
        _appWindow.Closing += OnWindowClosing;
        window.Closed += OnWindowClosed;
    }

    private void Restore()
    {
        if (_appWindow is null)
        {
            return;
        }

        var saved = Load();
        var requestedPoint = saved is null
            ? new PointInt32(int.MaxValue, int.MaxValue)
            : new PointInt32(saved.X, saved.Y);
        var displayArea = saved is null
            ? DisplayArea.Primary
            : DisplayArea.GetFromPoint(requestedPoint, DisplayAreaFallback.Primary);
        var workArea = displayArea.WorkArea;
        var dpi = GetTargetDpi(saved, workArea);
        var width = ScaleAndClamp(saved?.WidthInDips ?? DefaultWidth, dpi, MinimumWidth, workArea.Width);
        var height = ScaleAndClamp(saved?.HeightInDips ?? DefaultHeight, dpi, MinimumHeight, workArea.Height);

        var x = saved is null
            ? workArea.X + (workArea.Width - width) / 2
            : Math.Clamp(saved.X, workArea.X, workArea.X + workArea.Width - width);
        var y = saved is null
            ? workArea.Y + (workArea.Height - height) / 2
            : Math.Clamp(saved.Y, workArea.Y, workArea.Y + workArea.Height - height);

        _lastNormalBounds = new RectInt32(x, y, width, height);
        _appWindow.MoveAndResize(_lastNormalBounds);

        if (saved?.IsMaximized == true && _appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.Maximize();
        }
    }

    private int GetTargetDpi(WindowPlacement? saved, RectInt32 workArea)
    {
        if (saved is null)
        {
            return (int)NativeMethods.GetDpiForWindow(_windowHandle);
        }

        var point = new NativeMethods.Point
        {
            X = Math.Clamp(saved.X, workArea.X, workArea.X + Math.Max(0, workArea.Width - 1)),
            Y = Math.Clamp(saved.Y, workArea.Y, workArea.Y + Math.Max(0, workArea.Height - 1))
        };
        var monitor = NativeMethods.MonitorFromPoint(point, 2);
        return monitor != nint.Zero && NativeMethods.GetDpiForMonitor(monitor, 0, out var dpiX, out _) == 0
            ? (int)dpiX
            : (int)NativeMethods.GetDpiForWindow(_windowHandle);
    }

    private static int ScaleAndClamp(double dips, int dpi, int minimumDips, int maximumPixels)
    {
        var scaled = (int)Math.Round(Math.Max(minimumDips, dips) * dpi / 96d);
        return Math.Clamp(scaled, Math.Min(minimumDips, maximumPixels), maximumPixels);
    }

    private void OnWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidSizeChange && !_enforcingMinimumSize)
        {
            var dpi = Math.Max(96u, NativeMethods.GetDpiForWindow(_windowHandle));
            var minimumWidth = (int)Math.Round(MinimumWidth * dpi / 96d);
            var minimumHeight = (int)Math.Round(MinimumHeight * dpi / 96d);
            if (sender.Size.Width < minimumWidth || sender.Size.Height < minimumHeight)
            {
                _enforcingMinimumSize = true;
                sender.Resize(new SizeInt32(
                    Math.Max(sender.Size.Width, minimumWidth),
                    Math.Max(sender.Size.Height, minimumHeight)));
                _enforcingMinimumSize = false;
            }
        }

        if ((args.DidPositionChange || args.DidSizeChange || args.DidPresenterChange)
            && sender.Presenter is OverlappedPresenter presenter
            && presenter.State == OverlappedPresenterState.Restored)
        {
            CaptureNormalBounds();
        }
    }

    private void CaptureNormalBounds()
    {
        if (_appWindow is not null)
        {
            _lastNormalDpi = Math.Max(96u, NativeMethods.GetDpiForWindow(_windowHandle));
            _lastNormalBounds = new RectInt32(
                _appWindow.Position.X,
                _appWindow.Position.Y,
                _appWindow.Size.Width,
                _appWindow.Size.Height);
        }
    }

    private void OnWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        SaveCurrentPlacement(sender);
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        if (_appWindow is not null)
        {
            SaveCurrentPlacement(_appWindow);
        }
    }

    private void SaveCurrentPlacement(AppWindow sender)
    {
        if (_hasSaved)
        {
            return;
        }

        _hasSaved = true;
        var maximized = sender.Presenter is OverlappedPresenter presenter
            && presenter.State == OverlappedPresenterState.Maximized;
        Save(new WindowPlacement
        {
            X = _lastNormalBounds.X,
            Y = _lastNormalBounds.Y,
            WidthInDips = _lastNormalBounds.Width * 96d / _lastNormalDpi,
            HeightInDips = _lastNormalBounds.Height * 96d / _lastNormalDpi,
            IsMaximized = maximized
        });
    }

    private WindowPlacement? Load()
    {
        try
        {
            return File.Exists(_settingsPath)
                ? JsonSerializer.Deserialize<WindowPlacement>(File.ReadAllText(_settingsPath))
                : null;
        }
        catch (JsonException)
        {
            return null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    private void Save(WindowPlacement placement)
    {
        try
        {
            var directory = Path.GetDirectoryName(_settingsPath)!;
            Directory.CreateDirectory(directory);
            var temporaryPath = _settingsPath + ".tmp";
            File.WriteAllText(temporaryPath, JsonSerializer.Serialize(placement));
            File.Move(temporaryPath, _settingsPath, true);
        }
        catch (IOException)
        {
            // Window settings are optional; shutdown must never be blocked by persistence.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private sealed class WindowPlacement
    {
        public int X { get; set; }
        public int Y { get; set; }
        public double WidthInDips { get; set; }
        public double HeightInDips { get; set; }
        public bool IsMaximized { get; set; }
    }

    private static class NativeMethods
    {
        [StructLayout(LayoutKind.Sequential)]
        internal struct Point
        {
            internal int X;
            internal int Y;
        }

        [DllImport("user32.dll")]
        internal static extern uint GetDpiForWindow(nint windowHandle);

        [DllImport("user32.dll")]
        internal static extern nint MonitorFromPoint(Point point, uint flags);

        [DllImport("shcore.dll")]
        internal static extern int GetDpiForMonitor(nint monitor, int dpiType, out uint dpiX, out uint dpiY);
    }
}
