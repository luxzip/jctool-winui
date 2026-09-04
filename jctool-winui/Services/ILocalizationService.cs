namespace JcTool.WinUI.Services;

public interface ILocalizationService
{
    string Get(string key);
    string Format(string key, params object[] arguments);
}
