#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#include "po_catalog.h"

// One .po file per language in content/localization (docs/localization.md).
// msgid is the key and msgstr the text. A missing translation falls back to
// the source language file, then to "#Key". Main thread only.

inline constexpr const char *kLocalizationDirectory = "localization";

// "auto" picks the first OS language that has a .po file.
bool LocalizationSetLanguage(std::string_view language);
const std::string &LocalizationGetRequestedLanguage();
const std::string &LocalizationGetLanguage();
void LocalizationReload();
void LocalizationShutdown();

void LocalizationSetSourceLanguage(std::string_view language);
const std::string &LocalizationGetSourceLanguage();

std::vector<std::string> LocalizationGetAvailableLanguages();

// Bumped on every language change or reload.
uint64_t LocalizationGetGeneration();

// "pt-br" -> "pt_BR"
std::string LocalizationNormalizeLanguageCode(std::string_view code);

bool LocalizationHasKey(std::string_view key);

// Keys may be passed with or without the leading '#'.
std::string Tr(std::string_view key);
std::string TrPlural(std::string_view key, uint64_t n);

// Replaces {0}, {1}... so translations can reorder arguments. Other braces are left alone.
std::string LocFormat(std::string_view pattern, std::initializer_list<std::string_view> args);
