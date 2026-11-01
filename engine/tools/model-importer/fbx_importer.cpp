#include "fbx_importer.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "gltf_importer.h"
#include <windows.h>

namespace
{
    static std::string QuoteArgument(const std::string& value)
    {
        return "\"" + value + "\"";
    }

    static std::string GetEnvironmentVariable(const char* name)
    {
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
            return {};

        std::string result(value, valueLength > 0 ? valueLength - 1 : 0);
        free(value);
        return result;
    }

    static std::string GetExecutableDirectory()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return {};

        return std::filesystem::path(std::string(modulePath, length)).parent_path().string();
    }

    static std::string GetWindowsErrorMessage(DWORD errorCode)
    {
        char* messageBuffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageA(flags, nullptr, errorCode, 0, (LPSTR)&messageBuffer, 0, nullptr);
        if (length == 0 || messageBuffer == nullptr)
            return "Windows error " + std::to_string(errorCode);

        std::string message(messageBuffer, length);
        LocalFree(messageBuffer);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            message.pop_back();
        return message;
    }

    static std::string FindFBX2glTFExecutable()
    {
        namespace fs = std::filesystem;

        const std::string executableDirectory = GetExecutableDirectory();
        if (!executableDirectory.empty())
        {
            const std::vector<std::string> siblingCandidates = {
                "FBX2glTF-windows-x64.exe",
                "FBX2glTF"
            };

            for (const std::string& candidate : siblingCandidates)
            {
                const fs::path path = fs::path(executableDirectory) / candidate;
                if (fs::exists(path))
                    return path.lexically_normal().string();
            }
        }

        const std::string envPath = GetEnvironmentVariable("FBX2GLTF_PATH");
        if (!envPath.empty() && fs::exists(envPath))
            return envPath;

        const std::vector<std::string> relativeCandidates = {
            // Layout installed by external/download_external.(ps1|sh)
#if defined(_WIN32)
            "external/FBX2glTF/win64/FBX2glTF.exe",
#else
            "external/FBX2glTF/linux64/FBX2glTF",
#endif
            "FBX2glTF-windows-x64.exe",
            "FBX2glTF",
            "external/FBX2glTF/FBX2glTF-windows-x64.exe",
            "external/FBX2glTF/FBX2glTF",
            "external/FBX2glTF/bin/FBX2glTF-windows-x64.exe",
            "external/FBX2glTF/bin/FBX2glTF",
            "tools/FBX2glTF/FBX2glTF-windows-x64.exe",
            "tools/FBX2glTF/FBX2glTF"
        };

        fs::path current = fs::current_path();
        for (;;)
        {
            for (const std::string& candidate : relativeCandidates)
            {
                const fs::path path = current / candidate;
                if (fs::exists(path))
                    return path.lexically_normal().string();
            }

            const fs::path parent = current.parent_path();
            if (parent == current)
                break;
            current = parent;
        }

        return "FBX2glTF";
    }

    static bool RunFBX2glTF(const std::string& executable, const std::string& inputPath, const std::string& outputPath, std::string& error)
    {
        const std::string command =
            QuoteArgument(executable) +
            " --binary --input " + QuoteArgument(inputPath) +
            " --output " + QuoteArgument(outputPath);

        STARTUPINFOA startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);

        PROCESS_INFORMATION processInfo = {};
        std::vector<char> commandLine(command.begin(), command.end());
        commandLine.push_back('\0');

        const BOOL created = CreateProcessA(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo
        );

        if (!created)
        {
            error = "Failed to launch FBX2glTF: " + GetWindowsErrorMessage(GetLastError());
            return false;
        }

        WaitForSingleObject(processInfo.hProcess, INFINITE);

        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
        {
            const std::string launchError = GetWindowsErrorMessage(GetLastError());
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            error = "Failed to query FBX2glTF exit code: " + launchError;
            return false;
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        if (exitCode != 0)
        {
            error = "FBX2glTF failed with exit code " + std::to_string(exitCode) +
                ". Set FBX2GLTF_PATH to the converter executable if it is not on PATH.";
            return false;
        }

        return true;
    }
}

bool ImportFbxToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error)
{
    namespace fs = std::filesystem;

    const std::string converter = FindFBX2glTFExecutable();
    const fs::path outputModelPath(outputPath);
    const fs::path tempOutputPath = outputModelPath.parent_path() / (outputModelPath.stem().string() + ".fbx2gltf_tmp.glb");

    if (!RunFBX2glTF(converter, inputPath, tempOutputPath.string(), error))
        return false;

    fs::path convertedGlbPath = tempOutputPath;
    if (!fs::exists(convertedGlbPath))
    {
        const fs::path fallbackPath = tempOutputPath.parent_path() / tempOutputPath.stem();
        if (fs::exists(fallbackPath))
            convertedGlbPath = fallbackPath;
    }

    if (!fs::exists(convertedGlbPath))
    {
        error = "FBX2glTF completed but did not produce the expected GLB output";
        return false;
    }

    const bool imported = ImportGltfToKModel(convertedGlbPath.string(), outputPath, error);
    fs::remove(convertedGlbPath);
    return imported;
}
