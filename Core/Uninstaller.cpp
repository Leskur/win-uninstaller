#include "pch.h"
#include "Uninstaller.h"
#include "Format.h"

#include <condition_variable>
#include <regex>
#include <shellapi.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

namespace WinUninstaller::Core
{
    namespace
    {
        std::mutex g_workerMu;
        std::condition_variable g_workerCv;
        int g_winrtUsers = 0;
        std::atomic<bool> g_workerStop{ false };
    }

    WinrtLease::WinrtLease()
    {
        std::lock_guard<std::mutex> lock(g_workerMu);
        ++g_winrtUsers;
    }

    WinrtLease::~WinrtLease()
    {
        std::lock_guard<std::mutex> lock(g_workerMu);
        if (g_winrtUsers > 0) --g_winrtUsers;
        if (g_winrtUsers == 0) g_workerCv.notify_all();
    }

    void RequestWorkerShutdown()
    {
        g_workerStop.store(true);
        g_workerCv.notify_all();
    }

    bool WorkerShutdownRequested()
    {
        return g_workerStop.load();
    }

    void WaitForWinrtWorkers()
    {
        std::unique_lock<std::mutex> lock(g_workerMu);
        g_workerCv.wait(lock, [] { return g_winrtUsers == 0; });
    }

    namespace
    {
        using ::WinUninstaller::Core::Format::ContainsIgnoreCase;

        std::wstring FindProductGuid(std::wstring const& rawCommand)
        {
            static std::wregex const guidRegex(
                L"\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\}");
            std::wsmatch match;
            if (!std::regex_search(rawCommand, match, guidRegex)) return {};
            return match.str();
        }

        std::wstring SystemMsiexecPath()
        {
            wchar_t systemDir[MAX_PATH]{};
            GetSystemDirectoryW(systemDir, MAX_PATH);
            return std::wstring(systemDir) + L"\\msiexec.exe";
        }

        bool IsDirectExistingExecutable(std::wstring const& fileName)
        {
            if (fileName.empty() || ContainsIgnoreCase(fileName, L"msiexec")) return false;
            bool looksLikePath =
                (fileName.size() >= 2 && fileName[1] == L':') ||
                fileName.find(L'\\') != std::wstring::npos ||
                fileName.find(L'/') != std::wstring::npos;
            if (!looksLikePath) return false;
            DWORD attrs = GetFileAttributesW(fileName.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        UninstallResult RunProcess(std::wstring const& fileName, std::wstring const& arguments)
        {
            if (WorkerShutdownRequested())
            {
                return { false, static_cast<int>(ERROR_CANCELLED), L"" };
            }

            std::wstring directory;
            auto slash = fileName.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 0) directory = fileName.substr(0, slash);

            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            sei.lpVerb = L"open";
            sei.lpFile = fileName.c_str();
            sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
            sei.lpDirectory = directory.empty() ? nullptr : directory.c_str();
            sei.nShow = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&sei) || !sei.hProcess)
            {
                return { false, static_cast<int>(GetLastError()), L"Failed to start uninstaller." };
            }

            for (;;)
            {
                DWORD wait = WaitForSingleObject(sei.hProcess, 200);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_FAILED)
                {
                    CloseHandle(sei.hProcess);
                    return { false, static_cast<int>(GetLastError()), L"Failed to wait for uninstaller." };
                }
                if (WorkerShutdownRequested())
                {
                    CloseHandle(sei.hProcess);
                    return { false, static_cast<int>(ERROR_CANCELLED), L"" };
                }
            }

            DWORD exitCode = 1;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);

            bool success = exitCode == 0 ||
                exitCode == ERROR_SUCCESS_REBOOT_REQUIRED ||
                exitCode == ERROR_SUCCESS_REBOOT_INITIATED;
            return {
                success,
                static_cast<int>(exitCode),
                success ? L"Uninstall completed." : L"Uninstaller exited with an error."
            };
        }

        UninstallResult RemoveMsixPackage(
            std::wstring const& packageFullName,
            std::function<void(unsigned percent)> const& onProgress)
        {
            if (packageFullName.empty())
            {
                return { false, -1, L"Missing package identity." };
            }

            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            }
            catch (...)
            {
            }

            try
            {
                winrt::Windows::Management::Deployment::PackageManager manager;
                auto operation = manager.RemovePackageAsync(packageFullName);
                operation.Progress(
                    [onProgress](auto const&, winrt::Windows::Management::Deployment::DeploymentProgress const& progress) {
                        if (!onProgress) return;
                        try
                        {
                            onProgress(progress.percentage);
                        }
                        catch (...)
                        {
                        }
                    });

                HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!done)
                {
                    return { false, static_cast<int>(GetLastError()), L"Failed to wait for package removal." };
                }
                operation.Completed([done](auto const&, winrt::Windows::Foundation::AsyncStatus) {
                    SetEvent(done);
                });
                WaitForSingleObject(done, INFINITE);
                CloseHandle(done);

                if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Error)
                {
                    return { false, static_cast<int>(operation.ErrorCode()), L"Failed to remove the package." };
                }
                if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
                {
                    return { false, static_cast<int>(ERROR_CANCELLED), L"Package removal was canceled." };
                }

                auto deployment = operation.GetResults();
                HRESULT hr = deployment.ExtendedErrorCode();
                if (FAILED(hr))
                {
                    return { false, static_cast<int>(hr), deployment.ErrorText().c_str() };
                }
                return { true, 0, L"Uninstall completed." };
            }
            catch (winrt::hresult_error const& error)
            {
                return { false, static_cast<int>(error.code()), error.message().c_str() };
            }
            catch (...)
            {
                return { false, -1, L"Failed to remove the package." };
            }
        }
    }

    UninstallResult ExecuteUninstall(
        AppInfo const& app,
        std::function<void(unsigned percent)> const& onProgress)
    {
        if (app.SourceType == AppSourceType::MsixPackage)
        {
            return RemoveMsixPackage(app.Id, onProgress);
        }

        std::wstring rawCommand = Format::SelectUninstallCommand(
            app.UninstallString, app.QuietUninstallString);
        if (rawCommand.empty())
        {
            return { false, -1, L"No uninstall command." };
        }

        if (Format::IsOrphanedUninstallEntry(
                app.SourceType, app.WindowsInstaller,
                app.UninstallString, app.QuietUninstallString, app.InstallLocation))
        {
            return { false, static_cast<int>(ERROR_FILE_NOT_FOUND), L"Uninstaller executable is missing." };
        }

        auto cmd = Format::SplitUninstallCommand(rawCommand);
        if (IsDirectExistingExecutable(cmd.first))
        {
            return RunProcess(cmd.first, cmd.second);
        }

        auto guid = FindProductGuid(rawCommand);
        if (!guid.empty())
        {
            return RunProcess(SystemMsiexecPath(), L"/x " + guid);
        }

        if (ContainsIgnoreCase(rawCommand, L"msiexec") || ContainsIgnoreCase(cmd.first, L"msiexec"))
        {
            return RunProcess(SystemMsiexecPath(), cmd.second);
        }

        return RunProcess(cmd.first, cmd.second);
    }

    namespace
    {
        bool StartsWith(std::wstring const& text, wchar_t const* prefix)
        {
            size_t length = wcslen(prefix);
            return text.size() >= length && _wcsnicmp(text.c_str(), prefix, length) == 0;
        }

        bool TryParseUninstallSubkey(std::wstring path, HKEY& root, std::wstring& subkey)
        {
            root = nullptr;
            subkey.clear();
            while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
            {
                path.pop_back();
            }

            wchar_t const* hklm = L"HKEY_LOCAL_MACHINE\\";
            wchar_t const* hkcu = L"HKEY_CURRENT_USER\\";
            if (StartsWith(path, hklm))
            {
                root = HKEY_LOCAL_MACHINE;
                subkey = path.substr(wcslen(hklm));
            }
            else if (StartsWith(path, hkcu))
            {
                root = HKEY_CURRENT_USER;
                subkey = path.substr(wcslen(hkcu));
            }
            else
            {
                return false;
            }

            wchar_t const* prefixes[] = {
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\",
                L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\",
            };
            for (auto const* prefix : prefixes)
            {
                if (!StartsWith(subkey, prefix)) continue;
                std::wstring leaf = subkey.substr(wcslen(prefix));
                if (leaf.empty() || leaf.size() > 255) return false;
                if (leaf.find(L'\\') != std::wstring::npos || leaf.find(L'/') != std::wstring::npos)
                {
                    return false;
                }
                if (leaf == L"." || leaf == L"..") return false;
                return true;
            }
            return false;
        }
    }

    UninstallResult RemoveOrphanUninstallEntry(
        AppInfo const& app,
        std::vector<UninstallRegistryEntry> const& selected,
        bool entireKey)
    {
        if (!app.IsOrphaned || app.SourceType == AppSourceType::MsixPackage)
        {
            return { false, static_cast<int>(ERROR_INVALID_PARAMETER), L"Not a leftover entry." };
        }
        if (!entireKey && selected.empty())
        {
            return { false, static_cast<int>(ERROR_INVALID_PARAMETER), L"Nothing selected." };
        }

        HKEY root = nullptr;
        std::wstring subkey;
        if (!TryParseUninstallSubkey(app.RegistryKeyPath, root, subkey))
        {
            return { false, static_cast<int>(ERROR_INVALID_PARAMETER), L"Registry path is not an uninstall entry." };
        }

        if (entireKey)
        {
            LONG rc = RegDeleteTreeW(root, subkey.c_str());
            if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND)
            {
                return { true, 0, L"" };
            }
            return { false, static_cast<int>(rc), L"Could not remove the uninstall entry." };
        }

        HKEY key = nullptr;
        REGSAM access = DELETE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY;
        LONG opened = RegOpenKeyExW(root, subkey.c_str(), 0, access, &key);
        if (opened != ERROR_SUCCESS)
        {
            return { false, static_cast<int>(opened), L"Could not open the uninstall entry." };
        }

        for (auto const& entry : selected)
        {
            if (entry.Name.find(L'\\') != std::wstring::npos ||
                entry.Name.find(L'/') != std::wstring::npos ||
                entry.Name == L"." || entry.Name == L"..")
            {
                RegCloseKey(key);
                return { false, static_cast<int>(ERROR_INVALID_PARAMETER), L"Registry entry name is not safe to remove." };
            }
            LONG rc = entry.Subkey
                ? RegDeleteTreeW(key, entry.Name.c_str())
                : RegDeleteValueW(key, entry.Name.c_str());
            if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
            {
                RegCloseKey(key);
                return { false, static_cast<int>(rc), L"Could not remove the uninstall entry." };
            }
        }

        RegCloseKey(key);
        return { true, 0, L"" };
    }

    std::vector<UninstallRegistryEntry> ListOrphanUninstallEntries(AppInfo const& app)
    {
        std::vector<UninstallRegistryEntry> entries;
        if (!app.IsOrphaned || app.SourceType == AppSourceType::MsixPackage) return entries;

        HKEY root = nullptr;
        std::wstring subkey;
        if (!TryParseUninstallSubkey(app.RegistryKeyPath, root, subkey)) return entries;

        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        {
            return entries;
        }

        auto trimText = [](std::wstring text) {
            while (!text.empty() && text.back() == L'\0') text.pop_back();
            constexpr size_t kMax = 300;
            if (text.size() > kMax)
            {
                text.resize(kMax);
                text += L"...";
            }
            for (auto& ch : text)
            {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
            }
            return text;
        };

        for (DWORD index = 0;; ++index)
        {
            wchar_t name[256]{};
            DWORD nameLen = _countof(name);
            DWORD type = 0;
            DWORD dataSize = 0;
            LONG rc = RegEnumValueW(key, index, name, &nameLen, nullptr, &type, nullptr, &dataSize);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;

            UninstallRegistryEntry entry;
            entry.Name = name;
            if (type == REG_DWORD && dataSize >= sizeof(DWORD))
            {
                DWORD value = 0;
                DWORD size = sizeof(value);
                if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS)
                {
                    entry.Text = std::to_wstring(value);
                }
            }
            else if (type == REG_QWORD && dataSize >= sizeof(unsigned long long))
            {
                unsigned long long value = 0;
                DWORD size = sizeof(value);
                if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS)
                {
                    entry.Text = std::to_wstring(value);
                }
            }
            else if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
            {
                DWORD size = dataSize;
                if (size > 0 && size < 64 * 1024)
                {
                    std::wstring data(size / sizeof(wchar_t) + 1, L'\0');
                    if (RegQueryValueExW(
                            key, name, nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(data.data()), &size) == ERROR_SUCCESS)
                    {
                        if (type == REG_MULTI_SZ)
                        {
                            std::wstring joined;
                            wchar_t const* cursor = data.c_str();
                            while (*cursor)
                            {
                                if (!joined.empty()) joined += L" | ";
                                joined += cursor;
                                cursor += wcslen(cursor) + 1;
                            }
                            entry.Text = trimText(std::move(joined));
                        }
                        else
                        {
                            entry.Text = trimText(data.c_str());
                        }
                    }
                }
                else
                {
                    entry.Binary = true;
                    entry.BinaryBytes = dataSize;
                }
            }
            else
            {
                entry.Binary = true;
                entry.BinaryBytes = dataSize;
            }
            entries.push_back(std::move(entry));
        }

        for (DWORD index = 0;; ++index)
        {
            wchar_t name[256]{};
            DWORD nameLen = _countof(name);
            LONG rc = RegEnumKeyExW(key, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;
            UninstallRegistryEntry entry;
            entry.Name = name;
            entry.Subkey = true;
            entries.push_back(std::move(entry));
        }

        RegCloseKey(key);
        return entries;
    }
}
