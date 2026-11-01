#pragma once

#include <string>

#include "engine_types.h"

bool LoggerInitialize(String executablePath);
void LoggerShutdown();

void EngineLog(const std::string& message);
void EngineLogf(const char* fmt, ...);

void GameLog(const std::string& message);
void GameLogf(const char* fmt, ...);

std::string GetCurrentLogFilePath();
