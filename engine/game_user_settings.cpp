#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "game_user_settings.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include "command_line.h"
#include "engine_configuration.h"
#include "file_paths.h"
#include "logger.h"
#include "render_backend.h"
#include "window.h"

namespace
{
    GameUserSettings *gActiveSettings = nullptr;

    constexpr float kMinResolutionScale = 10.0f;
    constexpr float kMaxResolutionScale = 100.0f;
    constexpr float kMaxFrameRateLimit = 1000.0f;

    const char *WindowModeToString(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Fullscreen:
            return "Fullscreen";
        case WindowMode::WindowedFullscreen:
            return "WindowedFullscreen";
        default:
            return "Windowed";
        }
    }

    WindowMode WindowModeFromString(const std::string &text, WindowMode fallback)
    {
        if (text == "Fullscreen" || text == "0")
            return WindowMode::Fullscreen;
        if (text == "WindowedFullscreen" || text == "1")
            return WindowMode::WindowedFullscreen;
        if (text == "Windowed" || text == "2")
            return WindowMode::Windowed;
        return fallback;
    }

    // Values that are actually applied: saved settings plus command line overrides.
    struct EffectiveVideoMode
    {
        IntPoint resolution;
        WindowMode mode;
    };
}

GameUserSettings *GameUserSettings::GetGameUserSettings()
{
    return gActiveSettings;
}

void GameUserSettings::SetActiveInstance(bool active)
{
    if (active)
        gActiveSettings = this;
    else if (gActiveSettings == this)
        gActiveSettings = nullptr;
}

void GameUserSettings::AttachEngine(AppWindow *window, IRenderer *renderer, const AppConfig *defaults)
{
    m_window = window;
    m_renderer = renderer;
    m_defaults = defaults;
}

const AppConfig &GameUserSettings::Defaults() const
{
    static const AppConfig fallback = {};
    return m_defaults ? *m_defaults : fallback;
}

// --- Screen -----------------

void GameUserSettings::SetScreenResolution(IntPoint resolution)
{
    m_resolution = resolution;
}

IntPoint GameUserSettings::GetDesktopResolution() const
{
    return ::GetDesktopResolution(m_displayIndex);
}

std::vector<IntPoint> GameUserSettings::GetSupportedFullscreenResolutions() const
{
    return ::GetSupportedFullscreenResolutions(m_displayIndex);
}

void GameUserSettings::SetFullscreenMode(WindowMode mode)
{
    m_fullscreenMode = mode;
}

void GameUserSettings::SetPreferredFullscreenMode(WindowMode mode)
{
    m_preferredFullscreenMode = mode == WindowMode::Windowed ? WindowMode::WindowedFullscreen : mode;
}

void GameUserSettings::SetDisplayIndex(int displayIndex)
{
    m_displayIndex = displayIndex;
}

void GameUserSettings::SetFrameRateLimit(float framesPerSecond)
{
    m_frameRateLimit = framesPerSecond;
}

void GameUserSettings::SetResolutionScaleValue(float percent)
{
    m_resolutionScale = percent;
}

void GameUserSettings::SetResolutionScaleNormalized(float normalized)
{
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    m_resolutionScale = kMinResolutionScale + (kMaxResolutionScale - kMinResolutionScale) * normalized;
}

float GameUserSettings::GetResolutionScaleNormalized() const
{
    return (std::clamp(m_resolutionScale, kMinResolutionScale, kMaxResolutionScale) - kMinResolutionScale) /
           (kMaxResolutionScale - kMinResolutionScale);
}

IntPoint GameUserSettings::GetRenderResolution() const
{
    const float scale = std::clamp(m_resolutionScale, kMinResolutionScale, kMaxResolutionScale) / 100.0f;
    const AppConfig &defaults = Defaults();
    return {
        std::max(1, static_cast<int>(std::lround(defaults.renderWidth * scale))),
        std::max(1, static_cast<int>(std::lround(defaults.renderHeight * scale)))};
}

// --- Dirty state --------------

bool GameUserSettings::IsScreenResolutionDirty() const
{
    return !m_applied.valid || m_applied.resolution != m_resolution || m_applied.displayIndex != m_displayIndex;
}

bool GameUserSettings::IsFullscreenModeDirty() const
{
    return !m_applied.valid || m_applied.fullscreenMode != m_fullscreenMode;
}

bool GameUserSettings::IsVSyncDirty() const
{
    return !m_applied.valid || m_applied.vsync != m_vsync;
}

bool GameUserSettings::IsDirty() const
{
    return IsScreenResolutionDirty() || IsFullscreenModeDirty() || IsVSyncDirty();
}

// --- Apply

void GameUserSettings::ApplySettings(bool checkForCommandLineOverrides)
{
    ApplyResolutionSettings(checkForCommandLineOverrides);
    ApplyNonResolutionSettings();

    if (checkForCommandLineOverrides)
    {
        bool vsync = m_vsync;
        if (CommandLineHasFlag("vsync"))
            vsync = true;
        if (CommandLineHasFlag("novsync"))
            vsync = false;
        if (m_renderer && vsync != m_vsync)
            m_renderer->SetVSync(vsync);
    }

    SaveSettings();
}

void GameUserSettings::ApplyResolutionSettings(bool checkForCommandLineOverrides)
{
    ValidateSettings();

    EffectiveVideoMode effective = {m_resolution, m_fullscreenMode};
    if (checkForCommandLineOverrides)
    {
        CommandLineGetInt("ResX", effective.resolution.x);
        CommandLineGetInt("ResY", effective.resolution.y);
        if (CommandLineHasFlag("windowed"))
            effective.mode = WindowMode::Windowed;
        if (CommandLineHasFlag("borderless") || CommandLineHasFlag("windowedfullscreen"))
            effective.mode = WindowMode::WindowedFullscreen;
        if (CommandLineHasFlag("fullscreen"))
            effective.mode = WindowMode::Fullscreen;
    }

    if (m_window)
    {
        SetAppWindowMode(*m_window, effective.mode, effective.resolution, m_displayIndex);
        if (m_renderer && IsAppWindowRenderable(*m_window))
            m_renderer->Resize(m_window->pixelWidth, m_window->pixelHeight);
    }

    m_applied.resolution = m_resolution;
    m_applied.fullscreenMode = m_fullscreenMode;
    m_applied.displayIndex = m_displayIndex;
    m_applied.valid = true;
}

void GameUserSettings::ApplyNonResolutionSettings()
{
    ValidateSettings();

    if (m_renderer)
    {
        m_renderer->SetVSync(m_vsync);
        const IntPoint renderResolution = GetRenderResolution();
        m_renderer->SetRenderResolution(renderResolution.x, renderResolution.y);
    }
    m_applied.vsync = m_vsync;
    // The frame rate limit is read by the engine loop every frame.
}

void GameUserSettings::ConfirmVideoMode()
{
    m_lastConfirmedResolution = m_resolution;
    m_lastConfirmedFullscreenMode = m_fullscreenMode;
}

void GameUserSettings::RevertVideoMode()
{
    m_resolution = m_lastConfirmedResolution;
    m_fullscreenMode = m_lastConfirmedFullscreenMode;
}

void GameUserSettings::ResetToCurrentSettings()
{
    if (!m_applied.valid)
        return;
    m_resolution = m_applied.resolution;
    m_fullscreenMode = m_applied.fullscreenMode;
    m_displayIndex = m_applied.displayIndex;
    m_vsync = m_applied.vsync;
}

// --- Defaults / validation ------

void GameUserSettings::SetToDefaults()
{
    const AppConfig &defaults = Defaults();
    m_resolution = {defaults.windowWidth, defaults.windowHeight};
    m_lastConfirmedResolution = m_resolution;
    m_fullscreenMode = defaults.windowMode;
    m_lastConfirmedFullscreenMode = m_fullscreenMode;
    m_preferredFullscreenMode = WindowMode::WindowedFullscreen;
    m_displayIndex = 0;
    m_vsync = defaults.vsync;
    m_frameRateLimit = defaults.frameRateLimit;
    m_resolutionScale = 100.0f;
    m_preferredRenderer = RendererBackend::Auto;
}

void GameUserSettings::ValidateSettings()
{
    const int displayCount = GetDisplayCount();
    if (m_displayIndex < 0 || (displayCount > 0 && m_displayIndex >= displayCount))
        m_displayIndex = 0;

    // Resolutions: keep windowed sizes inside the desktop; zero means "desktop".
    const IntPoint desktop = ::GetDesktopResolution(m_displayIndex);
    auto fix = [&](IntPoint &value)
    {
        if (value.x <= 0 || value.y <= 0)
            value = desktop;
        value.x = std::clamp(value.x, 320, std::max(320, desktop.x * 2));
        value.y = std::clamp(value.y, 240, std::max(240, desktop.y * 2));
    };
    fix(m_resolution);
    fix(m_lastConfirmedResolution);
    if (m_fullscreenMode == WindowMode::Windowed)
    {
        m_resolution.x = std::min(m_resolution.x, desktop.x);
        m_resolution.y = std::min(m_resolution.y, desktop.y);
    }

    if (m_preferredFullscreenMode == WindowMode::Windowed)
        m_preferredFullscreenMode = WindowMode::WindowedFullscreen;

    if (!std::isfinite(m_frameRateLimit) || m_frameRateLimit < 0.0f)
        m_frameRateLimit = 0.0f;
    m_frameRateLimit = std::min(m_frameRateLimit, kMaxFrameRateLimit);

    if (!std::isfinite(m_resolutionScale))
        m_resolutionScale = 100.0f;
    m_resolutionScale = std::clamp(m_resolutionScale, kMinResolutionScale, kMaxResolutionScale);
}

// --- Persistence ----------------

std::string GameUserSettings::GetSettingsFilePath() const
{
    // Per-user, writable location (e.g. %APPDATA%/<company>/<project>/ on Windows,
    // ~/.local/share/<company>/<project>/ on Linux).
    std::filesystem::path base;
    if (char *pref = SDL_GetPrefPath(ENGINE_COMPANY_NAME, ENGINE_PROJECT_NAME))
    {
        base = pref;
        SDL_free(pref);
    }
    else
    {
        base = GetExecutableDirectory() / "Saved";
    }
    return (base / "Config" / "GameUserSettings.ini").string();
}

std::string GameUserSettings::GetDefaultSettingsFilePath() const
{
    return (GetExecutableDirectory() / "config" / "DefaultGameUserSettings.ini").string();
}

void GameUserSettings::ReadFromIni(const IniFile &ini)
{
    m_resolution.x = ini.GetInt(kSection, "ResolutionSizeX", m_resolution.x);
    m_resolution.y = ini.GetInt(kSection, "ResolutionSizeY", m_resolution.y);
    m_lastConfirmedResolution.x = ini.GetInt(kSection, "LastUserConfirmedResolutionSizeX", m_resolution.x);
    m_lastConfirmedResolution.y = ini.GetInt(kSection, "LastUserConfirmedResolutionSizeY", m_resolution.y);
    m_fullscreenMode = WindowModeFromString(ini.GetString(kSection, "FullscreenMode"), m_fullscreenMode);
    m_lastConfirmedFullscreenMode = WindowModeFromString(ini.GetString(kSection, "LastConfirmedFullscreenMode"), m_fullscreenMode);
    m_preferredFullscreenMode = WindowModeFromString(ini.GetString(kSection, "PreferredFullscreenMode"), m_preferredFullscreenMode);
    m_displayIndex = ini.GetInt(kSection, "DisplayIndex", m_displayIndex);
    m_vsync = ini.GetBool(kSection, "bUseVSync", m_vsync);
    m_frameRateLimit = ini.GetFloat(kSection, "FrameRateLimit", m_frameRateLimit);
    m_resolutionScale = ini.GetFloat(kSection, "ResolutionScale", m_resolutionScale);

    RendererBackend renderer = m_preferredRenderer;
    const std::string rendererName = ini.GetString(kSection, "PreferredRenderer", RendererBackendName(m_preferredRenderer));
    if (ParseRendererBackend(rendererName.c_str(), renderer))
        m_preferredRenderer = renderer;
}

void GameUserSettings::WriteToIni(IniFile &ini) const
{
    ini.SetInt(kSection, "Version", kSettingsVersion);
    ini.SetInt(kSection, "ResolutionSizeX", m_resolution.x);
    ini.SetInt(kSection, "ResolutionSizeY", m_resolution.y);
    ini.SetInt(kSection, "LastUserConfirmedResolutionSizeX", m_lastConfirmedResolution.x);
    ini.SetInt(kSection, "LastUserConfirmedResolutionSizeY", m_lastConfirmedResolution.y);
    ini.SetString(kSection, "FullscreenMode", WindowModeToString(m_fullscreenMode));
    ini.SetString(kSection, "LastConfirmedFullscreenMode", WindowModeToString(m_lastConfirmedFullscreenMode));
    ini.SetString(kSection, "PreferredFullscreenMode", WindowModeToString(m_preferredFullscreenMode));
    ini.SetInt(kSection, "DisplayIndex", m_displayIndex);
    ini.SetBool(kSection, "bUseVSync", m_vsync);
    ini.SetFloat(kSection, "FrameRateLimit", m_frameRateLimit);
    ini.SetFloat(kSection, "ResolutionScale", m_resolutionScale);
    ini.SetString(kSection, "PreferredRenderer", RendererBackendName(m_preferredRenderer));
}

void GameUserSettings::LoadSettings(bool forceReload)
{
    if (m_loaded && !forceReload)
        return;

    SetToDefaults();

    // Project defaults shipped with the game, then the player's file on top.
    IniFile defaults;
    const bool haveDefaults = defaults.LoadFromFile(GetDefaultSettingsFilePath());

    IniFile user;
    const std::string userPath = GetSettingsFilePath();
    bool haveUser = user.LoadFromFile(userPath);
    if (haveUser && user.GetInt(kSection, "Version", 0) < kSettingsVersion)
    {
        EngineLogf("Settings file %s is from an older version; resetting to defaults.", userPath.c_str());
        user.Clear();
        haveUser = false;
    }

    m_ini = defaults;
    m_ini.Merge(user);

    ReadFromIni(m_ini);
    LoadCustomSettings(m_ini);
    ValidateSettings();
    m_loaded = true;

    EngineLogf("Game user settings loaded (%s%s): %dx%d %s, vsync %s, fps limit %.0f, resolution scale %.0f%%.",
               haveUser ? userPath.c_str() : "no user file",
               haveDefaults ? ", with project defaults" : "",
               m_resolution.x, m_resolution.y, WindowModeToString(m_fullscreenMode),
               m_vsync ? "on" : "off", m_frameRateLimit, m_resolutionScale);
}

void GameUserSettings::SaveSettings()
{
    WriteToIni(m_ini);
    SaveCustomSettings(m_ini);

    const std::string path = GetSettingsFilePath();
    if (!m_ini.SaveToFile(path))
        EngineLogf("Failed to save game user settings to %s.", path.c_str());
}

// --- Custom values -----------------

void GameUserSettings::SetCustomString(std::string_view key, std::string_view value) { m_ini.SetString(kCustomSection, key, value); }
void GameUserSettings::SetCustomFloat(std::string_view key, float value) { m_ini.SetFloat(kCustomSection, key, value); }
void GameUserSettings::SetCustomInt(std::string_view key, int value) { m_ini.SetInt(kCustomSection, key, value); }
void GameUserSettings::SetCustomBool(std::string_view key, bool value) { m_ini.SetBool(kCustomSection, key, value); }
std::string GameUserSettings::GetCustomString(std::string_view key, std::string_view fallback) const { return m_ini.GetString(kCustomSection, key, fallback); }
float GameUserSettings::GetCustomFloat(std::string_view key, float fallback) const { return m_ini.GetFloat(kCustomSection, key, fallback); }
int GameUserSettings::GetCustomInt(std::string_view key, int fallback) const { return m_ini.GetInt(kCustomSection, key, fallback); }
bool GameUserSettings::GetCustomBool(std::string_view key, bool fallback) const { return m_ini.GetBool(kCustomSection, key, fallback); }
