#pragma once
#include <string>

void BuildInfoSetProject(const char* name, const char* company, const char* version);
const char* BuildInfoName();
const char* BuildInfoCompany();
const char* BuildInfoVersion();
const char* BuildInfoCommit();
const char* BuildInfoConfig();

std::string BuildInfoDescription(); // "kes 1.0.0.0 (db344c6, Release, Windows)"
std::string BuildInfoReleaseName(); // "kes@1.0.0.0+db344c6", the crash report release
