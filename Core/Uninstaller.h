#pragma once

#include "AppInfo.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace WinUninstaller::Core
{
    struct UninstallResult
    {
        bool Success{ false };
        int ExitCode{ -1 };
        std::wstring Message;
    };

    struct UninstallRegistryEntry
    {
        std::wstring Name;
        std::wstring Text;
        bool Subkey{ false };
        bool Binary{ false };
        std::uint32_t BinaryBytes{ 0 };
    };

    class WinrtLease
    {
    public:
        WinrtLease();
        ~WinrtLease();
        WinrtLease(WinrtLease const&) = delete;
        WinrtLease& operator=(WinrtLease const&) = delete;
    };

    void RequestWorkerShutdown();
    bool WorkerShutdownRequested();
    void WaitForWinrtWorkers();

    UninstallResult ExecuteUninstall(
        AppInfo const& app,
        std::function<void(unsigned percent)> const& onProgress = {});
    UninstallResult RemoveOrphanUninstallEntry(
        AppInfo const& app,
        std::vector<UninstallRegistryEntry> const& selected,
        bool entireKey);
    std::vector<UninstallRegistryEntry> ListOrphanUninstallEntries(AppInfo const& app);
}
