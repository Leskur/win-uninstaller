#include "pch.h"
#include "AppScanner.h"
#include "Format.h"

#include <set>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Management.Deployment.h>

namespace WinUninstaller::Core
{
    namespace
    {
        using ::WinUninstaller::Core::Format::ContainsIgnoreCase;

        bool StartsWithIgnoreCase(std::wstring const& value, std::wstring const& prefix)
        {
            if (value.size() < prefix.size()) return false;
            return _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
        }

        bool IsUpdateOrHotfixRelease(std::wstring const& releaseType)
        {
            if (releaseType.empty()) return false;
            return ContainsIgnoreCase(releaseType, L"Update") ||
                ContainsIgnoreCase(releaseType, L"Hotfix") ||
                ContainsIgnoreCase(releaseType, L"Security Update") ||
                ContainsIgnoreCase(releaseType, L"Service Pack");
        }

        bool IsUpdateOrHotfixName(std::wstring const& displayName)
        {
            auto name = displayName;
            while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
            if (name.empty()) return false;
            if (StartsWithIgnoreCase(name, L"KB") && name.size() > 2 && iswdigit(name[2]))
            {
                return true;
            }
            return StartsWithIgnoreCase(name, L"Security Update") ||
                StartsWithIgnoreCase(name, L"Update for") ||
                StartsWithIgnoreCase(name, L"Hotfix for") ||
                StartsWithIgnoreCase(name, L"Service Pack");
        }

        UninstallEngineType DetectEngine(
            std::wstring const& uninstallString,
            std::wstring const& quietString,
            bool isMsi)
        {
            if (isMsi) return UninstallEngineType::Msi;
            auto combined = uninstallString + L" " + quietString;
            if (ContainsIgnoreCase(combined, L"unins000") || ContainsIgnoreCase(combined, L"Inno"))
            {
                return UninstallEngineType::InnoSetup;
            }
            if (ContainsIgnoreCase(combined, L"uninst.exe") ||
                ContainsIgnoreCase(combined, L"uninstall.exe") ||
                ContainsIgnoreCase(combined, L"NSIS"))
            {
                return UninstallEngineType::Nsis;
            }
            if (ContainsIgnoreCase(combined, L"isuninst.exe") ||
                ContainsIgnoreCase(combined, L"InstallShield"))
            {
                return UninstallEngineType::InstallShield;
            }
            return UninstallEngineType::Unknown;
        }

        std::wstring RegGetString(HKEY key, wchar_t const* name)
        {
            DWORD type = 0;
            DWORD size = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
                size == 0)
            {
                return {};
            }
            if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
            std::wstring value(size / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(key, name, nullptr, &type,
                    reinterpret_cast<LPBYTE>(value.data()), &size) != ERROR_SUCCESS)
            {
                return {};
            }
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            return value;
        }

        DWORD RegGetDword(HKEY key, wchar_t const* name, bool& ok)
        {
            DWORD type = 0;
            DWORD value = 0;
            DWORD size = sizeof(value);
            ok = RegQueryValueExW(key, name, nullptr, &type,
                     reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
                 type == REG_DWORD;
            return value;
        }

        std::wstring FormatRegistryKeyPath(
            HKEY root,
            REGSAM view,
            wchar_t const* subKeyPath,
            wchar_t const* name)
        {
            wchar_t const* rootName = L"HKEY_LOCAL_MACHINE";
            if (root == HKEY_CURRENT_USER) rootName = L"HKEY_CURRENT_USER";

            std::wstring sub = subKeyPath ? subKeyPath : L"";
            // Regedit shows the redirected 32-bit Software hive under WOW6432Node.
            if ((view & KEY_WOW64_32KEY) != 0 &&
                StartsWithIgnoreCase(sub, L"SOFTWARE\\") &&
                !StartsWithIgnoreCase(sub, L"SOFTWARE\\WOW6432Node\\"))
            {
                sub = L"SOFTWARE\\WOW6432Node\\" + sub.substr(9);
            }

            return std::wstring(rootName) + L"\\" + sub + L"\\" + name;
        }

        void ScanRegistryKey(
            HKEY root,
            REGSAM view,
            wchar_t const* subKeyPath,
            AppSourceType sourceType,
            std::vector<AppInfo>& apps,
            std::set<std::wstring>& seenIds)
        {
            HKEY uninstallKey = nullptr;
            REGSAM access = KEY_READ | view;
            if (RegOpenKeyExW(root, subKeyPath, 0, access, &uninstallKey) != ERROR_SUCCESS)
            {
                return;
            }

            for (DWORD index = 0;; ++index)
            {
                wchar_t name[256]{};
                DWORD nameLen = _countof(name);
                LONG enumResult = RegEnumKeyExW(
                    uninstallKey, index, name, &nameLen,
                    nullptr, nullptr, nullptr, nullptr);
                if (enumResult == ERROR_NO_MORE_ITEMS) break;
                if (enumResult != ERROR_SUCCESS) continue;

                try
                {
                    HKEY appKey = nullptr;
                    if (RegOpenKeyExW(uninstallKey, name, 0, KEY_READ, &appKey) != ERROR_SUCCESS)
                    {
                        continue;
                    }

                    auto displayName = RegGetString(appKey, L"DisplayName");
                    if (displayName.empty())
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    bool ok = false;
                    DWORD sysComp = RegGetDword(appKey, L"SystemComponent", ok);
                    if (ok && sysComp == 1)
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    auto parentDisplayName = RegGetString(appKey, L"ParentDisplayName");
                    if (!parentDisplayName.empty())
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    auto releaseType = RegGetString(appKey, L"ReleaseType");
                    if (IsUpdateOrHotfixRelease(releaseType) || IsUpdateOrHotfixName(displayName))
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    DWORD noRemove = RegGetDword(appKey, L"NoRemove", ok);
                    if (ok && noRemove == 1)
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    auto uninstallString = Format::ExpandEnvironmentString(RegGetString(appKey, L"UninstallString"));
                    auto quietUninstallString = Format::ExpandEnvironmentString(RegGetString(appKey, L"QuietUninstallString"));
                    if (uninstallString.empty() && quietUninstallString.empty())
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    std::wstring uniqueId = FormatRegistryKeyPath(root, view, subKeyPath, name);
                    if (!seenIds.insert(uniqueId).second)
                    {
                        RegCloseKey(appKey);
                        continue;
                    }

                    auto displayVersion = RegGetString(appKey, L"DisplayVersion");
                    auto publisher = RegGetString(appKey, L"Publisher");
                    auto installDateRaw = RegGetString(appKey, L"InstallDate");
                    if (installDateRaw.empty())
                    {
                        DWORD dateDword = RegGetDword(appKey, L"InstallDate", ok);
                        if (ok && dateDword > 0)
                        {
                            wchar_t buf[16]{};
                            swprintf_s(buf, L"%08u", dateDword);
                            installDateRaw = buf;
                        }
                    }
                    auto installLocation = Format::ExpandEnvironmentString(RegGetString(appKey, L"InstallLocation"));
                    auto displayIcon = RegGetString(appKey, L"DisplayIcon");

                    DWORD winInstaller = RegGetDword(appKey, L"WindowsInstaller", ok);
                    bool isMsi = (ok && winInstaller == 1) ||
                        ContainsIgnoreCase(uninstallString, L"msiexec");

                    std::int64_t sizeBytes = 0;
                    bool hasEstimatedSize = false;
                    DWORD estimatedKb = RegGetDword(appKey, L"EstimatedSize", ok);
                    if (ok && estimatedKb > 0)
                    {
                        hasEstimatedSize = true;
                        sizeBytes = static_cast<std::int64_t>(estimatedKb) * 1024;
                    }

                    try
                    {
                        installLocation = Format::InferInstallLocation(installLocation, uninstallString);
                    }
                    catch (...)
                    {
                    }

                    if (displayIcon.empty())
                    {
                        auto [uninstallExe, _] = Format::SplitUninstallCommand(uninstallString);
                        if (!uninstallExe.empty()) displayIcon = uninstallExe;
                    }

                    bool isOrphaned = false;
                    try
                    {
                        isOrphaned = Format::IsOrphanedUninstallEntry(
                            sourceType, isMsi, uninstallString, quietUninstallString, installLocation);
                    }
                    catch (...)
                    {
                    }
                    if (isOrphaned)
                    {
                        sizeBytes = 0;
                        hasEstimatedSize = false;
                    }

                    bool sizePending = !isOrphaned && !hasEstimatedSize && !installLocation.empty() &&
                        !Format::IsDriveRootPath(installLocation);

                    std::wstring installDate;
                    try
                    {
                        installDate = Format::ResolveInstallDate(
                            installDateRaw, installLocation, uninstallString);
                    }
                    catch (...)
                    {
                    }

                    AppInfo info{};
                    info.Id = name;
                    info.DisplayName = displayName;
                    info.DisplayVersion = displayVersion;
                    info.Publisher = publisher;
                    info.InstallDate = installDate;
                    info.EstimatedSizeBytes = sizeBytes;
                    info.FormattedSize = (isOrphaned || sizePending) ? L"-" : Format::FormatBytes(sizeBytes);
                    info.InstallLocation = installLocation;
                    info.UninstallString = uninstallString;
                    info.QuietUninstallString = quietUninstallString;
                    info.DisplayIconPath = displayIcon;
                    info.RegistryKeyPath = FormatRegistryKeyPath(root, view, subKeyPath, name);
                    info.SourceType = sourceType;
                    info.EngineType = DetectEngine(uninstallString, quietUninstallString, isMsi);
                    info.WindowsInstaller = isMsi;
                    info.IsOrphaned = isOrphaned;
                    info.SizePending = sizePending;
                    info.Architecture =
                        sourceType == AppSourceType::Registry32 ? L"x86" : L"x64";
                    apps.push_back(std::move(info));

                    RegCloseKey(appKey);
                }
                catch (...)
                {
                    // Skip corrupt ARP entries.
                }
            }

            RegCloseKey(uninstallKey);
        }

        bool IsSystemMsixPackage(
            winrt::Windows::ApplicationModel::Package const& package,
            std::wstring const& installedLocation)
        {
            if (ContainsIgnoreCase(installedLocation, L"\\Windows\\SystemApps\\"))
            {
                return true;
            }
            try
            {
                return package.SignatureKind() ==
                    winrt::Windows::ApplicationModel::PackageSignatureKind::System;
            }
            catch (...)
            {
                return false;
            }
        }

        bool FileExists(std::wstring const& path)
        {
            if (path.empty()) return false;
            DWORD attrs = GetFileAttributesW(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
        }

        std::wstring PercentDecode(std::wstring const& input)
        {
            std::wstring out;
            std::string pending;
            out.reserve(input.size());
            auto flush = [&]() {
                if (pending.empty()) return;
                int wideLen = MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS,
                    pending.data(), static_cast<int>(pending.size()), nullptr, 0);
                if (wideLen > 0)
                {
                    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
                    MultiByteToWideChar(
                        CP_UTF8, MB_ERR_INVALID_CHARS,
                        pending.data(), static_cast<int>(pending.size()), wide.data(), wideLen);
                    out += wide;
                }
                else
                {
                    for (unsigned char byte : pending) out.push_back(static_cast<wchar_t>(byte));
                }
                pending.clear();
            };
            auto hexValue = [](wchar_t ch) -> int {
                if (ch >= L'0' && ch <= L'9') return ch - L'0';
                if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
                if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < input.size(); ++i)
            {
                if (input[i] == L'%' && i + 2 < input.size())
                {
                    int high = hexValue(input[i + 1]);
                    int low = hexValue(input[i + 2]);
                    if (high >= 0 && low >= 0)
                    {
                        pending.push_back(static_cast<char>((high << 4) | low));
                        i += 2;
                        continue;
                    }
                }
                flush();
                out.push_back(input[i]);
            }
            flush();
            return out;
        }

        std::wstring NormalizeFsPath(std::wstring path)
        {
            for (auto& c : path)
            {
                if (c == L'/') c = L'\\';
            }
            return path;
        }

        std::wstring JoinPath(std::wstring const& root, std::wstring const& relative)
        {
            std::wstring rel = NormalizeFsPath(PercentDecode(relative));
            while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/'))
            {
                rel.erase(rel.begin());
            }
            if (root.empty()) return rel;
            if (rel.empty()) return root;
            if (root.back() == L'\\' || root.back() == L'/') return root + rel;
            return root + L'\\' + rel;
        }

        int ScoreMsixAssetCandidate(std::wstring const& fileName)
        {
            std::wstring lower = fileName;
            for (auto& c : lower) c = towlower(c);

            // Prefer real app-list icons; avoid splash / jumplist / contrast variants.
            if (lower.find(L"splash") != std::wstring::npos) return -100;
            if (lower.find(L"jumplist") != std::wstring::npos) return -100;
            if (lower.find(L"illustration") != std::wstring::npos) return -100;
            if (lower.find(L"contrast-") != std::wstring::npos) return -50;

            int score = 0;
            if (lower.find(L"applist") != std::wstring::npos) score += 80;
            if (lower.find(L"square44") != std::wstring::npos) score += 70;
            if (lower.find(L"storelogo") != std::wstring::npos) score += 60;
            if (lower.find(L"smalltile") != std::wstring::npos) score += 40;
            if (lower.find(L"medtile") != std::wstring::npos) score += 20;
            if (lower.find(L"targetsize-48") != std::wstring::npos) score += 30;
            else if (lower.find(L"targetsize-32") != std::wstring::npos) score += 25;
            else if (lower.find(L"targetsize-24") != std::wstring::npos) score += 20;
            else if (lower.find(L"scale-100") != std::wstring::npos) score += 15;
            else if (lower.find(L"scale-200") != std::wstring::npos) score += 10;
            if (lower.find(L"altform-unplated") != std::wstring::npos) score += 5;
            if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".ico") score += 10;
            return score;
        }

        std::wstring ResolveLogicalMsixAsset(std::wstring const& logicalPath)
        {
            if (logicalPath.empty()) return {};
            if (FileExists(logicalPath)) return logicalPath;

            auto slash = logicalPath.find_last_of(L"\\/");
            auto fileName = slash == std::wstring::npos ? logicalPath : logicalPath.substr(slash + 1);
            auto dir = slash == std::wstring::npos ? std::wstring{} : logicalPath.substr(0, slash);
            auto dot = fileName.rfind(L'.');
            if (dot == std::wstring::npos) return {};

            auto stem = fileName.substr(0, dot);
            auto ext = fileName.substr(dot);

            static wchar_t const* kSuffixes[] = {
                L".targetsize-48",
                L".targetsize-48_altform-unplated",
                L".targetsize-32",
                L".targetsize-24",
                L".targetsize-16",
                L".scale-100",
                L".scale-200",
                L".scale-125",
                L".scale-150",
                L".scale-400",
                L".targetsize-256",
                L".targetsize-256_altform-unplated",
            };

            for (auto suffix : kSuffixes)
            {
                auto candidate = dir.empty()
                    ? (stem + suffix + ext)
                    : (dir + L'\\' + stem + suffix + ext);
                if (FileExists(candidate)) return candidate;
            }

            // e.g. WindowsSecurityAppList.png -> WindowsSecurityAppList*.png
            std::wstring pattern = dir.empty()
                ? (stem + L"*" + ext)
                : (dir + L'\\' + stem + L"*" + ext);
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return {};

            std::wstring best;
            int bestScore = -1000;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                int score = ScoreMsixAssetCandidate(fd.cFileName);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = dir.empty() ? fd.cFileName : (dir + L'\\' + fd.cFileName);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            return bestScore >= 0 ? best : std::wstring{};
        }

        std::wstring ReadUtf8File(std::wstring const& path)
        {
            HANDLE h = CreateFileW(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return {};

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
            {
                CloseHandle(h);
                return {};
            }

            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read = 0;
            bool ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
            CloseHandle(h);
            if (!ok || read == 0) return {};
            bytes.resize(read);

            int wideLen = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (wideLen <= 0) return {};
            std::wstring wide(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideLen);
            return wide;
        }

        std::wstring ExtractXmlAttr(std::wstring const& xml, wchar_t const* attrName)
        {
            std::wstring needle = std::wstring(attrName) + L"=\"";
            auto pos = xml.find(needle);
            if (pos == std::wstring::npos)
            {
                needle = std::wstring(attrName) + L"='";
                pos = xml.find(needle);
                if (pos == std::wstring::npos) return {};
            }
            pos += needle.size();
            wchar_t quote = xml[pos - 1];
            auto end = xml.find(quote, pos);
            if (end == std::wstring::npos) return {};
            return NormalizeFsPath(PercentDecode(xml.substr(pos, end - pos)));
        }

        std::wstring ExtractXmlElementText(std::wstring const& xml, wchar_t const* elementName)
        {
            std::wstring open = std::wstring(L"<") + elementName + L">";
            auto pos = xml.find(open);
            if (pos == std::wstring::npos) return {};
            pos += open.size();
            std::wstring close = std::wstring(L"</") + elementName + L">";
            auto end = xml.find(close, pos);
            if (end == std::wstring::npos) return {};
            return NormalizeFsPath(PercentDecode(xml.substr(pos, end - pos)));
        }

        bool IsImageAssetName(std::wstring const& lowerName)
        {
            return lowerName.ends_with(L".png") ||
                lowerName.ends_with(L".jpg") ||
                lowerName.ends_with(L".jpeg") ||
                lowerName.ends_with(L".ico") ||
                lowerName.ends_with(L".bmp");
        }

        std::wstring FindMsixLogoFallback(std::wstring const& installedLocation)
        {
            if (installedLocation.empty()) return {};

            static wchar_t const* kCandidates[] = {
                L"Assets\\StoreLogo.png",
                L"Assets\\Square44x44Logo.png",
                L"Assets\\Square150x150Logo.png",
                L"Assets\\Logo.png",
                L"StoreLogo.png",
                L"Logo.png",
            };

            for (auto candidate : kCandidates)
            {
                auto resolved = ResolveLogicalMsixAsset(JoinPath(installedLocation, candidate));
                if (!resolved.empty()) return resolved;
            }

            std::wstring assets = JoinPath(installedLocation, L"Assets");
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((assets + L"\\*.*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return {};

            std::wstring best;
            int bestScore = 0;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = fd.cFileName;
                std::wstring lower = name;
                for (auto& c : lower) c = towlower(c);
                if (!IsImageAssetName(lower)) continue;

                int score = ScoreMsixAssetCandidate(name);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = assets + L'\\' + name;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            return best;
        }

        std::wstring ResolveMsixLogoFromManifest(std::wstring const& installedLocation)
        {
            auto manifestXml = ReadUtf8File(JoinPath(installedLocation, L"AppxManifest.xml"));
            if (manifestXml.empty()) return {};

            static wchar_t const* kAttrs[] = {
                L"Square44x44Logo",
                L"Square150x150Logo",
                L"Square71x71Logo",
            };
            for (auto attr : kAttrs)
            {
                auto relative = ExtractXmlAttr(manifestXml, attr);
                if (relative.empty()) continue;
                auto resolved = ResolveLogicalMsixAsset(JoinPath(installedLocation, relative));
                if (!resolved.empty()) return resolved;
            }

            auto logoElement = ExtractXmlElementText(manifestXml, L"Logo");
            if (logoElement.empty()) return {};
            return ResolveLogicalMsixAsset(JoinPath(installedLocation, logoElement));
        }

        void ScanMsixPackages(
            std::vector<AppInfo>& apps,
            std::set<std::wstring>& seenIds)
        {
            try
            {
                winrt::Windows::Management::Deployment::PackageManager pm;
                using PackageTypes = winrt::Windows::Management::Deployment::PackageTypes;
                auto packageTypes = static_cast<PackageTypes>(
                    static_cast<std::uint32_t>(PackageTypes::Main) |
                    static_cast<std::uint32_t>(PackageTypes::Optional) |
                    static_cast<std::uint32_t>(PackageTypes::Bundle) |
                    static_cast<std::uint32_t>(PackageTypes::Xap));
                auto packages = pm.FindPackagesForUserWithPackageTypes(L"", packageTypes);
                for (auto const& package : packages)
                {
                    try
                    {
                        if (package.IsFramework() || package.IsResourcePackage()) continue;

                        std::wstring displayName = package.Id().Name().c_str();
                        try
                        {
                            auto dn = package.DisplayName();
                            if (!dn.empty()) displayName = dn.c_str();
                        }
                        catch (...)
                        {
                        }
                        if (displayName.empty()) continue;

                        std::wstring packageFullName = package.Id().FullName().c_str();
                        if (!seenIds.insert(packageFullName).second) continue;

                        std::wstring publisher;
                        try
                        {
                            publisher = package.PublisherDisplayName().c_str();
                        }
                        catch (...)
                        {
                        }

                        auto version = package.Id().Version();
                        wchar_t versionStr[64]{};
                        swprintf_s(
                            versionStr, L"%u.%u.%u.%u",
                            version.Major, version.Minor, version.Build, version.Revision);

                        std::wstring installedLocation;
                        try
                        {
                            installedLocation = package.InstalledLocation().Path().c_str();
                        }
                        catch (...)
                        {
                        }

                        if (IsSystemMsixPackage(package, installedLocation)) continue;

                        std::wstring installDate;
                        try
                        {
                            auto installed = package.InstalledDate();
                            auto ft = winrt::clock::to_FILETIME(installed);
                            FILETIME fileTime{};
                            fileTime.dwLowDateTime = static_cast<DWORD>(ft.value & 0xFFFFFFFFu);
                            fileTime.dwHighDateTime = static_cast<DWORD>(ft.value >> 32);
                            SYSTEMTIME stUtc{}, stLocal{};
                            FileTimeToSystemTime(&fileTime, &stUtc);
                            SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLocal);
                            if (stLocal.wYear >= 1990)
                            {
                                wchar_t buf[32]{};
                                swprintf_s(buf, L"%04u/%02u/%02u",
                                    stLocal.wYear, stLocal.wMonth, stLocal.wDay);
                                installDate = buf;
                            }
                        }
                        catch (...)
                        {
                        }
                        if (installDate.empty())
                        {
                            installDate = Format::InferInstallDateFromFileSystem(
                                installedLocation, L"");
                        }

                        std::int64_t sizeBytes = 0;
                        bool sizePending = !installedLocation.empty() && !Format::IsDriveRootPath(installedLocation);
                        if (sizePending)
                        {
                            Format::TryGetCachedDirectorySize(installedLocation, sizeBytes);
                        }
                        AppInfo info{};
                        info.Id = packageFullName;
                        info.DisplayName = displayName;
                        info.DisplayVersion = versionStr;
                        info.Publisher = publisher;
                        info.InstallDate = installDate;
                        info.EstimatedSizeBytes = sizeBytes;
                        info.FormattedSize = (sizePending && sizeBytes <= 0) ? L"-" : Format::FormatBytes(sizeBytes);
                        info.InstallLocation = installedLocation;
                        info.DisplayIconPath = installedLocation;
                        info.SourceType = AppSourceType::MsixPackage;
                        info.SizePending = sizePending;
                        info.Architecture = L"neutral";
                        try
                        {
                            info.Architecture = std::to_wstring(
                                static_cast<int>(package.Id().Architecture()));
                        }
                        catch (...)
                        {
                        }
                        apps.push_back(std::move(info));
                    }
                    catch (...)
                    {
                    }
                }
            }
            catch (...)
            {
            }
        }
    }

    std::vector<AppInfo> EnumerateRegistryApplications()
    {
        std::vector<AppInfo> apps;
        apps.reserve(256);
        std::set<std::wstring> seenIds;

        constexpr wchar_t const* uninstallPath =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

        ScanRegistryKey(
            HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, uninstallPath,
            AppSourceType::Registry64, apps, seenIds);

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL)
        {
            ScanRegistryKey(
                HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, uninstallPath,
                AppSourceType::Registry32, apps, seenIds);
        }

        ScanRegistryKey(
            HKEY_CURRENT_USER, 0, uninstallPath,
            AppSourceType::RegistryUser, apps, seenIds);
        return apps;
    }

    std::vector<AppInfo> EnumerateMsixApplications()
    {
        std::vector<AppInfo> apps;
        std::set<std::wstring> seenIds;
        try
        {
            ScanMsixPackages(apps, seenIds);
        }
        catch (...)
        {
        }
        return apps;
    }

    std::wstring ResolveMsixLogoFile(std::wstring const& installedLocation)
    {
        auto fromManifest = ResolveMsixLogoFromManifest(installedLocation);
        if (!fromManifest.empty()) return fromManifest;
        return FindMsixLogoFallback(installedLocation);
    }

    std::vector<AppInfo> EnumerateApplications()
    {
        auto apps = EnumerateRegistryApplications();
        auto msix = EnumerateMsixApplications();
        apps.insert(
            apps.end(),
            std::make_move_iterator(msix.begin()),
            std::make_move_iterator(msix.end()));
        return apps;
    }
}
