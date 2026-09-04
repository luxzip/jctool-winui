namespace JcTool.WinUI.Services;

public sealed class LanguagePreferenceService
{
    public const string SystemLanguage = "system";
    private static readonly HashSet<string> SupportedLanguages =
        new(StringComparer.OrdinalIgnoreCase) { SystemLanguage, "zh-CN", "en-US" };
    private static readonly string SettingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "JcTool",
        "language.txt");

    public string EffectivePreference
    {
        get
        {
            var commandLineLanguage = Environment.GetCommandLineArgs()
                .FirstOrDefault(argument => argument.StartsWith("--language=", StringComparison.OrdinalIgnoreCase));
            var preference = commandLineLanguage?[11..] ?? Load();
            return SupportedLanguages.Contains(preference) ? preference : SystemLanguage;
        }
    }

    public string CurrentPreference
    {
        get => EffectivePreference;
    }

    public void Save(string preference)
    {
        if (!SupportedLanguages.Contains(preference))
        {
            throw new ArgumentOutOfRangeException(nameof(preference));
        }

        var directory = Path.GetDirectoryName(SettingsPath)!;
        Directory.CreateDirectory(directory);
        var temporaryPath = SettingsPath + ".tmp";
        File.WriteAllText(temporaryPath, preference);
        File.Move(temporaryPath, SettingsPath, true);
    }

    private static string Load()
    {
        try
        {
            return File.Exists(SettingsPath) ? File.ReadAllText(SettingsPath).Trim() : SystemLanguage;
        }
        catch (IOException)
        {
            return SystemLanguage;
        }
        catch (UnauthorizedAccessException)
        {
            return SystemLanguage;
        }
    }
}
