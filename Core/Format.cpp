#include "pch.h"
#include "Format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
namespace WinUninstaller::Core::Format
{
    bool ContainsIgnoreCase(std::wstring const& hay, std::wstring const& needle)
    {
        if (needle.empty()) return false;
        auto it = std::search(
            hay.begin(), hay.end(),
            needle.begin(), needle.end(),
            [](wchar_t a, wchar_t b) {
                return towlower(a) == towlower(b);
            });
        return it != hay.end();
    }

    namespace
    {
        std::wstring Trim(std::wstring s)
        {
            auto notSpace = [](wchar_t c) { return !iswspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            while (!s.empty() && (s.front() == L'"' || s.front() == L'\'')) s.erase(s.begin());
            while (!s.empty() && (s.back() == L'"' || s.back() == L'\'')) s.pop_back();
            return s;
        }

        bool TryGetConcreteUninstallerPath(std::wstring const& command, std::wstring& exePath)
        {
            exePath.clear();
            std::wstring candidate;
            if (!TryGetUninstallExecutable(command, candidate)) return false;
            candidate = Trim(candidate);
            bool looksLikePath =
                (candidate.size() >= 2 && candidate[1] == L':') ||
                candidate.find(L'\\') != std::wstring::npos ||
                candidate.find(L'/') != std::wstring::npos;
            if (!looksLikePath) return false;
            exePath = candidate;
            return true;
        }

        bool ContainsMsiOrProtocol(std::wstring const& command)
        {
            if (command.empty()) return false;
            return ContainsIgnoreCase(command, L"msiexec") ||
                command.find(L"://") != std::wstring::npos;
        }

        std::wstring FormatDisplayDate(SYSTEMTIME const& st)
        {
            wchar_t buf[32]{};
            swprintf_s(buf, L"%04u/%02u/%02u", st.wYear, st.wMonth, st.wDay);
            return buf;
        }

        bool TryParseYyyymmdd(std::wstring const& raw, SYSTEMTIME& st)
        {
            std::wstring digits;
            digits.reserve(8);
            for (wchar_t c : raw)
            {
                if (iswdigit(c)) digits.push_back(c);
                if (digits.size() >= 8) break;
            }
            if (digits.size() != 8) return false;
            int y = _wtoi(digits.substr(0, 4).c_str());
            int m = _wtoi(digits.substr(4, 2).c_str());
            int d = _wtoi(digits.substr(6, 2).c_str());
            if (y < 1990 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return false;
            ZeroMemory(&st, sizeof(st));
            st.wYear = static_cast<WORD>(y);
            st.wMonth = static_cast<WORD>(m);
            st.wDay = static_cast<WORD>(d);
            return true;
        }

        bool TryGetDirectoryCreationDate(std::wstring const& path, std::wstring& formatted)
        {
            formatted.clear();
            if (path.empty() || IsDriveRootPath(path)) return false;

            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(Trim(path).c_str(), GetFileExInfoStandard, &data))
            {
                return false;
            }
            SYSTEMTIME stUtc{}, stLocal{};
            FileTimeToSystemTime(&data.ftCreationTime, &stUtc);
            SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLocal);
            if (stLocal.wYear < 1990) return false;
            formatted = FormatDisplayDate(stLocal);
            return true;
        }
    }

    std::wstring FormatBytes(std::int64_t bytes)
    {
        if (bytes <= 0) return L"-";
        static wchar_t const* suffixes[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
        int place = 0;
        double value = static_cast<double>(bytes);
        while (value >= 1024.0 && place < 4)
        {
            value /= 1024.0;
            ++place;
        }
        wchar_t buf[64]{};
        if (place == 0)
            swprintf_s(buf, L"%.0f %s", value, suffixes[place]);
        else
            swprintf_s(buf, L"%.2f %s", value, suffixes[place]);
        return buf;
    }

    std::int64_t CalculateDirectorySize(std::wstring const& dirPath)
    {
        std::int64_t size = 0;
        if (!TryMeasureDirectorySize(dirPath, size)) return 0;
        return size;
    }

    bool TryMeasureDirectorySize(std::wstring const& dirPath, std::int64_t& sizeBytes)
    {
        sizeBytes = 0;
        auto root = Trim(dirPath);
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/'))
        {
            root.pop_back();
        }
        if (root.empty() || IsDriveRootPath(root)) return true;

        DWORD attrs = GetFileAttributesW(root.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return true;
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return true;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            HANDLE handle = CreateFileW(
                root.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return true;
            wchar_t finalPath[1024]{};
            DWORD length = GetFinalPathNameByHandleW(handle, finalPath, 1024, FILE_NAME_NORMALIZED);
            CloseHandle(handle);
            if (length == 0 || length >= 1024) return true;
            root = finalPath;
            if (root.rfind(L"\\\\?\\UNC\\", 0) == 0) root = L"\\\\" + root.substr(8);
            else if (root.rfind(L"\\\\?\\", 0) == 0) root = root.substr(4);
            while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
            if (root.empty() || IsDriveRootPath(root)) return true;
            attrs = GetFileAttributesW(root.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return true;
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;
        }

        std::vector<std::wstring> stack;
        stack.push_back(root);
        bool sawRoot = false;
        std::int64_t total = 0;
        while (!stack.empty())
        {
            std::wstring dir = std::move(stack.back());
            stack.pop_back();
            WIN32_FIND_DATAW found{};
            std::wstring pattern = dir + L"\\*";
            HANDLE find = FindFirstFileExW(
                pattern.c_str(), FindExInfoBasic, &found,
                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE)
            {
                if (!sawRoot) return false;
                continue;
            }
            sawRoot = true;
            do
            {
                if (found.cFileName[0] == L'.' &&
                    (found.cFileName[1] == 0 || (found.cFileName[1] == L'.' && found.cFileName[2] == 0)))
                {
                    continue;
                }
                if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    stack.push_back(dir + L'\\' + found.cFileName);
                    continue;
                }
                ULARGE_INTEGER fileSize{};
                fileSize.LowPart = found.nFileSizeLow;
                fileSize.HighPart = found.nFileSizeHigh;
                total += static_cast<std::int64_t>(fileSize.QuadPart);
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }
        sizeBytes = total;
        return true;
    }

    namespace
    {
        constexpr std::uint32_t kSizeCacheMagic = 0x315A5357;
        constexpr std::uint32_t kSizeCacheVersion = 2;

        std::mutex g_sizeCacheMu;
        bool g_sizeCacheLoaded = false;
        std::unordered_map<std::wstring, std::int64_t> g_sizeCache;

        std::wstring SizeCacheKey(std::wstring path)
        {
            path = Trim(path);
            while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
            {
                path.pop_back();
            }
            for (auto& ch : path) ch = towlower(ch);
            return path;
        }

        std::wstring SizeCacheFilePath()
        {
            wchar_t local[MAX_PATH]{};
            DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return {};
            return std::wstring(local) + L"\\Leskur\\WinUninstaller\\size-cache.bin";
        }

        bool ReadExact(HANDLE file, void* data, DWORD bytes)
        {
            DWORD read = 0;
            return ReadFile(file, data, bytes, &read, nullptr) && read == bytes;
        }

        bool WriteExact(HANDLE file, void const* data, DWORD bytes)
        {
            DWORD written = 0;
            return WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
        }

        void EnsureSizeCacheLoaded()
        {
            if (g_sizeCacheLoaded) return;
            g_sizeCacheLoaded = true;
            auto path = SizeCacheFilePath();
            if (path.empty()) return;
            HANDLE file = CreateFileW(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;

            std::uint32_t magic = 0;
            std::uint32_t version = 0;
            std::uint32_t count = 0;
            if (!ReadExact(file, &magic, sizeof(magic)) ||
                !ReadExact(file, &version, sizeof(version)) ||
                !ReadExact(file, &count, sizeof(count)) ||
                magic != kSizeCacheMagic || version != kSizeCacheVersion || count > 100000)
            {
                CloseHandle(file);
                return;
            }

            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::int64_t size = 0;
                std::uint32_t chars = 0;
                if (!ReadExact(file, &size, sizeof(size)) || !ReadExact(file, &chars, sizeof(chars))) break;
                if (chars == 0 || chars > 32767) break;
                std::wstring stored(chars, L'\0');
                if (!ReadExact(file, stored.data(), static_cast<DWORD>(stored.size() * sizeof(wchar_t)))) break;
                g_sizeCache.insert_or_assign(SizeCacheKey(stored), size);
            }
            CloseHandle(file);
        }

        void SaveSizeCache()
        {
            auto path = SizeCacheFilePath();
            if (path.empty()) return;
            auto slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos) return;
            SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);

            std::wstring temp = path + L".tmp";
            HANDLE file = CreateFileW(
                temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;

            std::uint32_t magic = kSizeCacheMagic;
            std::uint32_t version = kSizeCacheVersion;
            std::uint32_t count = static_cast<std::uint32_t>(g_sizeCache.size());
            bool ok = WriteExact(file, &magic, sizeof(magic)) &&
                WriteExact(file, &version, sizeof(version)) &&
                WriteExact(file, &count, sizeof(count));
            for (auto const& [key, size] : g_sizeCache)
            {
                if (!ok) break;
                std::uint32_t chars = static_cast<std::uint32_t>(key.size());
                ok = WriteExact(file, &size, sizeof(size)) &&
                    WriteExact(file, &chars, sizeof(chars)) &&
                    WriteExact(file, key.data(), static_cast<DWORD>(key.size() * sizeof(wchar_t)));
            }
            CloseHandle(file);
            if (!ok)
            {
                DeleteFileW(temp.c_str());
                return;
            }
            MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }

    bool TryGetCachedDirectorySize(std::wstring const& dirPath, std::int64_t& sizeBytes)
    {
        sizeBytes = 0;
        auto key = SizeCacheKey(dirPath);
        if (key.empty()) return false;
        std::lock_guard<std::mutex> lock(g_sizeCacheMu);
        EnsureSizeCacheLoaded();
        auto it = g_sizeCache.find(key);
        if (it == g_sizeCache.end()) return false;
        sizeBytes = it->second;
        return true;
    }

    void StoreCachedDirectorySize(std::wstring const& dirPath, std::int64_t sizeBytes)
    {
        auto key = SizeCacheKey(dirPath);
        if (key.empty() || key.size() > 32767) return;
        std::lock_guard<std::mutex> lock(g_sizeCacheMu);
        EnsureSizeCacheLoaded();
        g_sizeCache.insert_or_assign(std::move(key), sizeBytes);
    }

    void FlushDirectorySizeCache()
    {
        std::lock_guard<std::mutex> lock(g_sizeCacheMu);
        if (!g_sizeCacheLoaded) return;
        SaveSizeCache();
    }

    std::wstring NormalizeDate(std::wstring const& rawDate)
    {
        auto raw = Trim(rawDate);
        if (raw.empty()) return {};
        SYSTEMTIME st{};
        if (TryParseYyyymmdd(raw, st)) return FormatDisplayDate(st);
        return {};
    }

    std::wstring ResolveInstallDate(
        std::wstring const& registryInstallDate,
        std::wstring const& installLocation,
        std::wstring const& uninstallString)
    {
        auto fromRegistry = NormalizeDate(registryInstallDate);
        if (!fromRegistry.empty()) return fromRegistry;
        return InferInstallDateFromFileSystem(installLocation, uninstallString);
    }

    std::wstring InferInstallDateFromFileSystem(
        std::wstring const& installLocation,
        std::wstring const& uninstallString)
    {
        std::wstring exe;
        if (TryGetUninstallExecutable(uninstallString, exe))
        {
            try
            {
                fs::path p(exe);
                auto dir = p.parent_path().wstring();
                std::wstring formatted;
                if (TryGetDirectoryCreationDate(dir, formatted)) return formatted;
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data))
                {
                    SYSTEMTIME stUtc{}, stLocal{};
                    FileTimeToSystemTime(&data.ftCreationTime, &stUtc);
                    SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLocal);
                    if (stLocal.wYear >= 1990) return FormatDisplayDate(stLocal);
                }
            }
            catch (...)
            {
            }
        }

        std::wstring formatted;
        if (TryGetDirectoryCreationDate(installLocation, formatted)) return formatted;
        return {};
    }

    std::wstring ExpandEnvironmentString(std::wstring const& raw)
    {
        if (raw.find(L'%') == std::wstring::npos) return raw;
        DWORD needed = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
        if (needed == 0) return raw;
        std::wstring expanded(needed, L'\0');
        DWORD written = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), needed);
        if (written == 0) return raw;
        while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
        return expanded;
    }

    std::pair<std::wstring, std::wstring> SplitUninstallCommand(std::wstring const& rawCommand)
    {
        auto raw = Trim(ExpandEnvironmentString(rawCommand));
        if (raw.empty()) return {};

        if (raw.front() == L'"')
        {
            auto next = raw.find(L'"', 1);
            if (next != std::wstring::npos && next > 1)
            {
                return { raw.substr(1, next - 1), Trim(raw.substr(next + 1)) };
            }
        }

        static wchar_t const* exts[] = { L".exe", L".cmd", L".bat", L".msi" };
        for (auto ext : exts)
        {
            auto lower = raw;
            for (auto& c : lower) c = towlower(c);
            std::wstring extLower = ext;
            size_t searchFrom = 0;
            while (searchFrom < lower.size())
            {
                auto epos = lower.find(extLower, searchFrom);
                if (epos == std::wstring::npos || epos == 0) break;
                auto end = epos + extLower.size();
                bool boundary = end == lower.size() || iswspace(lower[end]) || lower[end] == L'"';
                if (boundary)
                {
                    return {
                        raw.substr(0, end),
                        end < raw.size() ? Trim(raw.substr(end)) : L""
                    };
                }
                searchFrom = epos + 1;
            }
        }

        auto space = raw.find(L' ');
        if (space != std::wstring::npos)
        {
            return { raw.substr(0, space), Trim(raw.substr(space + 1)) };
        }
        return { raw, L"" };
    }

    bool TryGetUninstallExecutable(std::wstring const& uninstallString, std::wstring& exePath)
    {
        exePath.clear();
        if (uninstallString.empty()) return false;
        auto raw = Trim(ExpandEnvironmentString(uninstallString));
        if (ContainsIgnoreCase(raw, L"msiexec")) return false;
        if (raw.find(L"://") != std::wstring::npos) return false;
        auto [fileName, _] = SplitUninstallCommand(raw);
        exePath = Trim(fileName);
        return !exePath.empty();
    }

    bool CommandIsRunnable(std::wstring const& command)
    {
        if (Trim(command).empty()) return false;
        std::wstring exe;
        if (!TryGetConcreteUninstallerPath(command, exe)) return true;
        DWORD attrs = GetFileAttributesW(exe.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring SelectUninstallCommand(
        std::wstring const& uninstallString,
        std::wstring const& quietUninstallString)
    {
        if (CommandIsRunnable(uninstallString)) return uninstallString;
        if (CommandIsRunnable(quietUninstallString)) return quietUninstallString;
        return {};
    }

    bool IsOrphanedUninstallEntry(
        AppSourceType sourceType,
        bool windowsInstaller,
        std::wstring const& uninstallString,
        std::wstring const& quietUninstallString,
        std::wstring const&)
    {
        if (sourceType == AppSourceType::MsixPackage) return false;
        if (windowsInstaller) return false;
        if (ContainsMsiOrProtocol(uninstallString) || ContainsMsiOrProtocol(quietUninstallString))
        {
            return false;
        }

        bool sawConcretePath = false;
        bool anyFileExists = false;
        auto consider = [&](std::wstring const& command) {
            std::wstring exe;
            if (!TryGetConcreteUninstallerPath(command, exe)) return;
            sawConcretePath = true;
            DWORD attrs = GetFileAttributesW(exe.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                anyFileExists = true;
            }
        };
        consider(uninstallString);
        consider(quietUninstallString);
        return sawConcretePath && !anyFileExists;
    }

    bool CanExecuteUninstall(AppInfo const& app)
    {
        if (app.SourceType == AppSourceType::MsixPackage)
        {
            return !app.Id.empty();
        }
        return !SelectUninstallCommand(app.UninstallString, app.QuietUninstallString).empty();
    }

    bool IsDriveRootPath(std::wstring const& path)
    {
        if (path.empty()) return false;
        try
        {
            auto normalized = fs::absolute(Trim(path)).wstring();
            while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/'))
            {
                normalized.pop_back();
            }
            auto root = fs::path(normalized).root_path().wstring();
            while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
            {
                root.pop_back();
            }
            return _wcsicmp(normalized.c_str(), root.c_str()) == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    std::wstring InferInstallLocation(
        std::wstring const& installLocation,
        std::wstring const& uninstallString)
    {
        auto location = Trim(ExpandEnvironmentString(installLocation));
        if (!location.empty() && !IsDriveRootPath(location)) return location;

        std::wstring exe;
        if (TryGetUninstallExecutable(uninstallString, exe))
        {
            try
            {
                auto dir = fs::path(exe).parent_path().wstring();
                if (!dir.empty() && !IsDriveRootPath(dir)) return dir;
            }
            catch (...)
            {
            }
        }
        return {};
    }

    bool TryResolveExistingDirectory(std::wstring const& path, std::wstring& existingPath)
    {
        existingPath.clear();
        if (path.empty()) return false;
        try
        {
            std::wstring current = Trim(ExpandEnvironmentString(path));
            while (!current.empty() && (current.back() == L'\\' || current.back() == L'/'))
            {
                current.pop_back();
            }
            if (current.empty()) return false;

            auto attributes = [](std::wstring const& candidate) -> DWORD {
                return GetFileAttributesW(candidate.c_str());
            };
            DWORD attrs = attributes(current);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                existingPath = current;
                return true;
            }

            auto slash = current.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash == 0) return false;
            std::wstring leaf = current.substr(slash + 1);
            std::wstring parent = current.substr(0, slash);
            bool leafIsFile = attrs != INVALID_FILE_ATTRIBUTES || leaf.find(L'.') != std::wstring::npos;
            if (!leafIsFile) return false;
            if (parent.size() == 2 && parent[1] == L':') return false;

            DWORD parentAttrs = attributes(parent);
            if (parentAttrs != INVALID_FILE_ATTRIBUTES && (parentAttrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                existingPath = parent;
                return true;
            }
        }
        catch (...)
        {
        }
        return false;
    }
}
