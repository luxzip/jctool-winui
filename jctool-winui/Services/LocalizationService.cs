using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.Windows.ApplicationModel.Resources;

namespace JcTool.WinUI.Services;

public sealed class LocalizationService : ILocalizationService
{
    private readonly ResourceLoader? _resourceLoader;
    private readonly ResourceMap? _resourceMap;
    private readonly ResourceContext? _resourceContext;

    public LocalizationService(string languagePreference)
    {
        if (languagePreference == LanguagePreferenceService.SystemLanguage)
        {
            _resourceLoader = new ResourceLoader();
            return;
        }

        var resourceManager = new ResourceManager();
        _resourceContext = resourceManager.CreateResourceContext();
        _resourceContext.QualifierValues["Language"] = languagePreference;
        _resourceMap = resourceManager.MainResourceMap.GetSubtree("Resources");
    }

    public string Get(string key)
    {
        try
        {
            var value = _resourceLoader is not null
                ? GetFromResourceLoader(key)
                : GetFromResourceMap(key);
            return string.IsNullOrEmpty(value) ? key : value;
        }
        catch (COMException)
        {
            return key;
        }
    }

    private string GetFromResourceLoader(string key)
    {
        try
        {
            return _resourceLoader!.GetString(key);
        }
        catch (COMException) when (key.Contains('.'))
        {
            return _resourceLoader!.GetString(key.Replace('.', '/'));
        }
    }

    private string GetFromResourceMap(string key)
    {
        var candidate = _resourceMap!.TryGetValue(key, _resourceContext!);
        if (candidate is null && key.Contains('.'))
        {
            candidate = _resourceMap.TryGetValue(key.Replace('.', '/'), _resourceContext!);
        }
        return candidate?.ValueAsString ?? string.Empty;
    }

    public string Format(string key, params object[] arguments)
    {
        return string.Format(CultureInfo.CurrentCulture, Get(key), arguments);
    }
}
