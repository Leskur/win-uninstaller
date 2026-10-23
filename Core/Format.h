#pragma once

#include "AppInfo.h"

#include <string>
#include <utility>

namespace WinUninstaller::Core::Format
{
    bool ContainsIgnoreCase(std::wstring const& hay, std::wstring const& needle);
    std::wstring FormatBytes(std::int64_t bytes);
    std::int64_t CalculateDirectorySize(std::wstring const& dirPath);
    bool TryMeasureDirectorySize(std::wstring const& dirPath, std::int64_t& sizeBytes);
    bool TryGetCachedDirectorySize(std::wstring const& dirPath, std::int64_t& sizeBytes);
    void StoreCachedDirectorySize(std::wstring const& dirPath, std::int64_t sizeBytes);
    void FlushDirectorySizeCache();
    std::wstring NormalizeDate(std::wstring const& rawDate);
    std::wstring ResolveInstallDate(
        std::wstring const& registryInstallDate,
        std::wstring const& installLocation,
        std::wstring const& uninstallString);
    std::wstring InferInstallDateFromFileSystem(
        std::wstring const& installLocation,
        std::wstring const& uninstallString);
    std::pair<std::wstring, std::wstring> SplitUninstallCommand(std::wstring const& rawCommand);
    bool TryGetUninstallExecutable(std::wstring const& uninstallString, std::wstring& exePath);
    bool CommandIsRunnable(std::wstring const& command);
    std::wstring SelectUninstallCommand(
        std::wstring const& uninstallString,
        std::wstring const& quietUninstallString);
    std::wstring ExpandEnvironmentString(std::wstring const& raw);
    bool IsOrphanedUninstallEntry(
        AppSourceType sourceType,
        bool windowsInstaller,
        std::wstring const& uninstallString,
        std::wstring const& quietUninstallString,
        std::wstring const& installLocation);
    bool CanExecuteUninstall(AppInfo const& app);
    bool IsDriveRootPath(std::wstring const& path);
    std::wstring InferInstallLocation(
        std::wstring const& installLocation,
        std::wstring const& uninstallString);
    bool TryResolveExistingDirectory(std::wstring const& path, std::wstring& existingPath);
}
