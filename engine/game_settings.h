#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "ini_file.h"
#include "shared.h"

struct AppWindow;
class IRenderer;

// Player-facing engine settings, modelled on Unreal's UGameUserSettings.
//
// Typical settings-menu flow (same as in Unreal):
//
//   GameSettings* settings = GetGameSettings();
//   settings->SetScreenResolution({ 1920, 1080 });
//   settings->SetFullscreenMode(WindowMode::WindowedFullscreen);
//   settings->SetVSyncEnabled(false);
//   settings->ApplySettings(false);          // apply + save
//   // ...ask "Keep these settings?"...
//   settings->ConfirmVideoMode();             // or RevertVideoMode() + ApplySettings(false)
//   settings->SaveSettings();
//
// Storage: <user config dir>/Config/GameSettings.ini (SDL_GetPrefPath),
// layered on top of <exe>/config/DefaultGameSettings.ini and the
// AppConfig defaults.
//
// Games add their own settings either with the Custom* key/value helpers or
// by subclassing and registering the class before EngineStart:
//
//   class MySettings : public GameSettings
//   {
//   public:
//       float masterVolume = 1.0f;
//   protected:
//       void SetToDefaults() override { GameSettings::SetToDefaults(); masterVolume = 1.0f; }
//       void LoadCustomSettings(const IniFile& ini) override { masterVolume = ini.GetFloat("MySettings", "MasterVolume", masterVolume); }
//       void SaveCustomSettings(IniFile& ini) const override { ini.SetFloat("MySettings", "MasterVolume", masterVolume); }
//   };
//   EngineSetGameSettingsClass<MySettings>(app);
//   ...
//   GetGameSettings<MySettings>()->masterVolume = 0.5f;
class GameSettings
{
public:
    // Bump to reset every player's settings file to defaults on the next launch
    // (e.g. after changing what a setting means).
    static constexpr int kSettingsVersion = 1;

    GameSettings() = default;
    virtual ~GameSettings() = default;

    // Engine-owned instance, valid between EngineStart and EngineShutdown.
    static GameSettings *GetGameSettings();

    // Screen
    void SetScreenResolution(IntPoint resolution);
    IntPoint GetScreenResolution() const { return m_resolution; }
    IntPoint GetLastConfirmedScreenResolution() const { return m_lastConfirmedResolution; }
    IntPoint GetDesktopResolution() const;
    std::vector<IntPoint> GetSupportedFullscreenResolutions() const;

    void SetFullscreenMode(WindowMode mode);
    WindowMode GetFullscreenMode() const { return m_fullscreenMode; }
    WindowMode GetLastConfirmedFullscreenMode() const { return m_lastConfirmedFullscreenMode; }
    // Fullscreen flavour used when toggling out of Windowed (Alt+Enter).
    void SetPreferredFullscreenMode(WindowMode mode);
    WindowMode GetPreferredFullscreenMode() const { return m_preferredFullscreenMode; }

    void SetDisplayIndex(int displayIndex);
    int GetDisplayIndex() const { return m_displayIndex; }

    void SetVSyncEnabled(bool enabled) { m_vsync = enabled; }
    bool IsVSyncEnabled() const { return m_vsync; }

    // 0 = unlimited.
    void SetFrameRateLimit(float framesPerSecond);
    float GetFrameRateLimit() const { return m_frameRateLimit; }

    // Internal render resolution as a percentage of AppConfig::renderWidth/Height
    // (Unreal's r.ScreenPercentage). Normalized maps 0..1 to the min..100% range.
    void SetResolutionScaleValue(float percent);
    float GetResolutionScaleValue() const { return m_resolutionScale; }
    void SetResolutionScaleNormalized(float normalized);
    float GetResolutionScaleNormalized() const;
    IntPoint GetRenderResolution() const;

    // Takes effect on the next launch (the renderer can't be swapped live).
    void SetPreferredRenderer(RendererBackend backend) { m_preferredRenderer = backend; }
    RendererBackend GetPreferredRenderer() const { return m_preferredRenderer; }

    // Dirty state (compared with what is currently applied)
    bool IsDirty() const;
    bool IsScreenResolutionDirty() const;
    bool IsFullscreenModeDirty() const;
    bool IsVSyncDirty() const;

    // Apply / persistence
    // ApplySettings = ApplyResolutionSettings + ApplyNonResolutionSettings + SaveSettings.
    // Command line overrides (-ResX= -ResY= -windowed -fullscreen -borderless
    // -vsync -novsync -fps=) affect what is applied but are never saved.
    virtual void ApplySettings(bool checkForCommandLineOverrides);
    virtual void ApplyResolutionSettings(bool checkForCommandLineOverrides);
    virtual void ApplyNonResolutionSettings();

    // Video mode confirmation, for "keep these settings?" prompts.
    void ConfirmVideoMode();
    void RevertVideoMode();

    virtual void LoadSettings(bool forceReload = false);
    virtual void SaveSettings();
    virtual void ValidateSettings();
    virtual void SetToDefaults();
    // Discards unapplied changes: reloads the values that are currently in effect.
    void ResetToCurrentSettings();

    std::string GetSettingsFilePath() const;
    std::string GetDefaultSettingsFilePath() const;

    // Custom key/value settings, saved in [CustomSettings]
    void SetCustomString(std::string_view key, std::string_view value);
    void SetCustomFloat(std::string_view key, float value);
    void SetCustomInt(std::string_view key, int value);
    void SetCustomBool(std::string_view key, bool value);
    std::string GetCustomString(std::string_view key, std::string_view fallback = {}) const;
    float GetCustomFloat(std::string_view key, float fallback) const;
    int GetCustomInt(std::string_view key, int fallback) const;
    bool GetCustomBool(std::string_view key, bool fallback) const;

    // Engine wiring (called by the engine, not by game code)
    void AttachEngine(AppWindow *window, IRenderer *renderer, const AppConfig *defaults);
    void SetActiveInstance(bool active);

protected:
    // Subclass hooks for game specific settings stored in the same file.
    virtual void LoadCustomSettings(const IniFile &ini) { (void)ini; }
    virtual void SaveCustomSettings(IniFile &ini) const { (void)ini; }

    static constexpr const char *kSection = "GameSettings";
    static constexpr const char *kCustomSection = "CustomSettings";

    const AppConfig &Defaults() const;
    AppWindow *Window() const { return m_window; }
    IRenderer *Renderer() const { return m_renderer; }

private:
    struct AppliedState
    {
        IntPoint resolution;
        WindowMode fullscreenMode = WindowMode::Windowed;
        int displayIndex = 0;
        bool vsync = true;
        bool valid = false;
    };

    void ReadFromIni(const IniFile &ini);
    void WriteToIni(IniFile &ini) const;

    // Saved values (Unreal key names in comments where they match).
    IntPoint m_resolution = {1280, 720};                                   // ResolutionSizeX/Y
    IntPoint m_lastConfirmedResolution = {1280, 720};                      // LastUserConfirmedResolutionSizeX/Y
    WindowMode m_fullscreenMode = WindowMode::Windowed;                    // FullscreenMode
    WindowMode m_lastConfirmedFullscreenMode = WindowMode::Windowed;       // LastConfirmedFullscreenMode
    WindowMode m_preferredFullscreenMode = WindowMode::WindowedFullscreen; // PreferredFullscreenMode
    int m_displayIndex = 0;
    bool m_vsync = true;              // bUseVSync
    float m_frameRateLimit = 0.0f;    // FrameRateLimit
    float m_resolutionScale = 100.0f; // ResolutionScale (percent)
    RendererBackend m_preferredRenderer = RendererBackend::Auto;

    IniFile m_ini; // whole document, so unknown sections survive a save
    bool m_loaded = false;
    AppliedState m_applied;

    AppWindow *m_window = nullptr;
    IRenderer *m_renderer = nullptr;
    const AppConfig *m_defaults = nullptr;
};

// Global accessor (Unreal: UGameUserSettings::GetGameUserSettings()).
inline GameSettings *GetGameSettings()
{
    return GameSettings::GetGameSettings();
}

template <typename T>
T *GetGameSettings()
{
    return dynamic_cast<T *>(GameSettings::GetGameSettings());
}
