#pragma once

#include <cstdint>
#include <string>

namespace WinUninstaller::Core
{
    enum class AppSourceType
    {
        Registry64,
        Registry32,
        RegistryUser,
        MsixPackage
    };

    enum class UninstallEngineType
    {
        Unknown,
        InnoSetup,
        Nsis,
        Msi,
        InstallShield
    };

    struct AppInfo
    {
        std::wstring Id;
        std::wstring DisplayName;
        std::wstring DisplayVersion;
        std::wstring Publisher;
        std::wstring InstallDate;
        std::int64_t EstimatedSizeBytes{ 0 };
        std::wstring FormattedSize;
        std::wstring InstallLocation;
        std::wstring UninstallString;
        std::wstring QuietUninstallString;
        std::wstring DisplayIconPath;
        std::wstring RegistryKeyPath;
        AppSourceType SourceType{ AppSourceType::Registry64 };
        UninstallEngineType EngineType{ UninstallEngineType::Unknown };
        bool WindowsInstaller{ false };
        bool IsOrphaned{ false };
        bool SizePending{ false };
        std::wstring Architecture{ L"x64" };
    };
}
