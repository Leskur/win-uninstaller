#pragma once

#include "AppInfo.h"

#include <vector>

namespace WinUninstaller::Core
{
    std::vector<AppInfo> EnumerateRegistryApplications();
    std::vector<AppInfo> EnumerateMsixApplications();
    std::vector<AppInfo> EnumerateApplications();
    std::wstring ResolveMsixLogoFile(std::wstring const& installedLocation);
}
