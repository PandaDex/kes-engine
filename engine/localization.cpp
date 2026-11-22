#include "localization.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include "logger.h"
#include "pak_file_system.h"
#include "profiler.h"

namespace
{
    std::string_view TrimView(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
    }
}

namespace
{
    struct LocalizationState
    {
        std::string sourceLanguage = "en";
        std::string requested = "auto";
        std::string active = "en";
        std::string activeFile;
        PoCatalog catalog;
        PoCatalog sourceCatalog; // only loaded when the active language isn't the source
        std::unordered_set<std::string> reportedMissing;
        uint64_t generation = 0;
    };

    LocalizationState& State()
    {
        static LocalizationState state;
        return state;
    }

    std::string BaseLanguage(const std::string& code)
    {
        const size_t underscore = code.find('_');
        return underscore == std::string::npos ? code : code.substr(0, underscore);
    }

    std::vector<std::pair<std::string, std::string>> ScanLanguageFiles()
    {
        std::vector<std::pair<std::string, std::string>> files;
        for (const std::string& file : ListAssetFiles(kLocalizationDirectory, ".po"))
        {
            const std::string code = LocalizationNormalizeLanguageCode(std::string_view(file).substr(0, file.size() - 3));
            if (!code.empty())
                files.emplace_back(code, file);
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    const std::string* FindFile(const std::vector<std::pair<std::string, std::string>>& files, const std::string& code)
    {
        auto found = std::find_if(files.begin(), files.end(), [&](const auto& file)
            { return file.first == code; });
        return found != files.end() ? &found->second : nullptr;
    }

    // Candidates for a requested code: "pt_BR" -> "pt_BR", "pt".
    void AddCandidates(std::vector<std::string>& candidates, const std::string& code)
    {
        if (code.empty())
            return;
        if (std::find(candidates.begin(), candidates.end(), code) == candidates.end())
            candidates.push_back(code);
        const std::string base = BaseLanguage(code);
        if (std::find(candidates.begin(), candidates.end(), base) == candidates.end())
            candidates.push_back(base);
    }

    std::vector<std::string> SystemLanguages()
    {
        std::vector<std::string> languages;
        int count = 0;
        if (SDL_Locale** locales = SDL_GetPreferredLocales(&count))
        {
            for (int index = 0; index < count; ++index)
            {
                if (!locales[index] || !locales[index]->language)
                    continue;
                std::string code = locales[index]->language;
                if (locales[index]->country && locales[index]->country[0])
                    code += std::string("_") + locales[index]->country;
                languages.push_back(LocalizationNormalizeLanguageCode(code));
            }
            SDL_free(locales);
        }
        return languages;
    }

    bool LoadCatalogFile(const std::string& file, PoCatalog& catalog)
    {
        const std::string path = std::string(kLocalizationDirectory) + "/" + file;
        std::string text;
        std::string error;
        if (!LoadAssetFileText(path.c_str(), text, &error))
        {
            EngineLogf("[Localization] Could not read %s: %s", path.c_str(), error.c_str());
            return false;
        }
        if (!catalog.Parse(text, &error))
        {
            EngineLogf("[Localization] %s: %s", path.c_str(), error.c_str());
            return false;
        }
        if (catalog.GetSkippedCount() > 0)
            EngineLogf("[Localization] %s: ignored %zu entries with a msgctxt (msgid is the key).", path.c_str(), catalog.GetSkippedCount());
        return true;
    }

    void ReportMissing(LocalizationState& state, std::string_view key)
    {
        if (state.reportedMissing.emplace(key).second)
            EngineLogf("[Localization] Missing key #%.*s (%s).", static_cast<int>(key.size()), key.data(), state.active.c_str());
    }

    const PoCatalog::Entry* FindSourceEntry(const LocalizationState& state, std::string_view key)
    {
        if (const PoCatalog::Entry* entry = state.sourceCatalog.FindEntry(key))
            return entry;
        return state.catalog.FindEntry(key);
    }
}

std::string LocalizationNormalizeLanguageCode(std::string_view code)
{
    code = TrimView(code);
    // Drop an encoding or modifier: "pl_PL.UTF-8", "sr_RS@latin".
    const size_t cut = code.find_first_of(".@");
    if (cut != std::string_view::npos)
        code = code.substr(0, cut);

    std::string normalized;
    bool region = false;
    for (char ch : code)
    {
        if (ch == '-' || ch == '_')
        {
            if (normalized.empty() || region)
                break;
            region = true;
            normalized.push_back('_');
            continue;
        }
        if (!std::isalnum(static_cast<unsigned char>(ch)))
            return {};
        normalized.push_back(static_cast<char>(region ? std::toupper(static_cast<unsigned char>(ch))
                                                      : std::tolower(static_cast<unsigned char>(ch))));
    }
    if (!normalized.empty() && normalized.back() == '_')
        normalized.pop_back();
    return normalized;
}

bool LocalizationSetLanguage(std::string_view language)
{
    KES_ZONE_FN(IO);
    LocalizationState& state = State();
    const std::string trimmed(TrimView(language));
    const bool automatic = trimmed.empty() || EqualsIgnoreCase(trimmed, "auto");
    state.requested = automatic ? "auto" : LocalizationNormalizeLanguageCode(trimmed);

    std::vector<std::string> candidates;
    if (automatic)
    {
        for (const std::string& system : SystemLanguages())
            AddCandidates(candidates, system);
    }
    else
    {
        AddCandidates(candidates, state.requested);
    }

    const std::vector<std::pair<std::string, std::string>> files = ScanLanguageFiles();
    std::string chosenCode = state.sourceLanguage;
    for (const std::string& candidate : candidates)
    {
        if (candidate == state.sourceLanguage || FindFile(files, candidate))
        {
            chosenCode = candidate;
            break;
        }
    }

    bool ok = true;
    if (!automatic && chosenCode == state.sourceLanguage && BaseLanguage(state.requested) != BaseLanguage(state.sourceLanguage))
    {
        EngineLogf("[Localization] No %s/%s.po; using the source language.", kLocalizationDirectory, state.requested.c_str());
        ok = false;
    }

    PoCatalog catalog;
    std::string chosenFile;
    if (const std::string* file = FindFile(files, chosenCode))
    {
        if (LoadCatalogFile(*file, catalog))
            chosenFile = *file;
        else
            ok = false;
    }
    if (chosenFile.empty() && chosenCode != state.sourceLanguage)
    {
        chosenCode = state.sourceLanguage;
        if (const std::string* file = FindFile(files, chosenCode); file && LoadCatalogFile(*file, catalog))
            chosenFile = *file;
    }

    // Keys the active language doesn't translate show the source language text.
    PoCatalog sourceCatalog;
    if (chosenCode != state.sourceLanguage)
    {
        if (const std::string* file = FindFile(files, state.sourceLanguage))
            LoadCatalogFile(*file, sourceCatalog);
        else
            EngineLogf("[Localization] No %s/%s.po for the source language; untranslated keys show as #Key.",
                kLocalizationDirectory, state.sourceLanguage.c_str());
    }

    state.active = chosenCode;
    state.activeFile = chosenFile;
    state.catalog = std::move(catalog);
    state.sourceCatalog = std::move(sourceCatalog);
    state.reportedMissing.clear();
    ++state.generation;
    EngineLogf("[Localization] Language: %s (%s, %zu keys).", state.active.c_str(),
        chosenFile.empty() ? "no .po file" : chosenFile.c_str(), state.catalog.GetEntryCount());
    return ok;
}

const std::string& LocalizationGetRequestedLanguage()
{
    return State().requested;
}

const std::string& LocalizationGetLanguage()
{
    return State().active;
}

void LocalizationReload()
{
    LocalizationState& state = State();
    LocalizationSetLanguage(state.requested);
}

void LocalizationShutdown()
{
    LocalizationState& state = State();
    state.catalog.Clear();
    state.sourceCatalog.Clear();
    state.reportedMissing.clear();
    state.active = state.sourceLanguage;
    state.activeFile.clear();
    state.requested = "auto";
    ++state.generation;
}

void LocalizationSetSourceLanguage(std::string_view language)
{
    const std::string code = LocalizationNormalizeLanguageCode(language);
    if (code.empty())
        return;
    LocalizationState& state = State();
    if (state.activeFile.empty())
        state.active = code;
    state.sourceLanguage = code;
}

const std::string& LocalizationGetSourceLanguage()
{
    return State().sourceLanguage;
}

std::vector<std::string> LocalizationGetAvailableLanguages()
{
    std::vector<std::string> languages{ State().sourceLanguage };
    for (const auto& file : ScanLanguageFiles())
    {
        if (std::find(languages.begin(), languages.end(), file.first) == languages.end())
            languages.push_back(file.first);
    }
    return languages;
}

uint64_t LocalizationGetGeneration()
{
    return State().generation;
}

bool LocalizationHasKey(std::string_view key)
{
    return FindSourceEntry(State(), LocalizationStripKey(key)) != nullptr;
}

std::string Tr(std::string_view key)
{
    LocalizationState& state = State();
    const std::string_view stripped = LocalizationStripKey(key);
    if (const std::string* text = state.catalog.Find(stripped))
        return *text;
    if (const std::string* text = state.sourceCatalog.Find(stripped))
        return *text;
    ReportMissing(state, stripped);
    return "#" + std::string(stripped);
}

std::string TrPlural(std::string_view key, uint64_t n)
{
    LocalizationState& state = State();
    const std::string_view stripped = LocalizationStripKey(key);
    if (const std::string* text = state.catalog.FindPlural(stripped, n))
        return *text;
    if (const std::string* text = state.sourceCatalog.FindPlural(stripped, n))
        return *text;
    ReportMissing(state, stripped);
    return "#" + std::string(stripped);
}

std::string LocFormat(std::string_view pattern, std::initializer_list<std::string_view> args)
{
    std::string result;
    result.reserve(pattern.size());
    size_t pos = 0;
    while (pos < pattern.size())
    {
        const char ch = pattern[pos];
        if (ch == '{')
        {
            size_t end = pos + 1;
            size_t index = 0;
            while (end < pattern.size() && end - pos <= 3 && std::isdigit(static_cast<unsigned char>(pattern[end])))
                index = index * 10 + static_cast<size_t>(pattern[end++] - '0');
            if (end > pos + 1 && end < pattern.size() && pattern[end] == '}' && index < args.size())
            {
                result.append(*(args.begin() + index));
                pos = end + 1;
                continue;
            }
        }
        result.push_back(ch);
        ++pos;
    }
    return result;
}
