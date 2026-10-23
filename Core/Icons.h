#pragma once

#include "AppInfo.h"

#include <Windows.h>

namespace WinUninstaller::Core
{
    // Returns a small icon (caller must DestroyIcon). Null on failure.
    // cx/cy are the desired pixel size. A negative icon index is a resource id.
    HICON LoadAppSmallIcon(AppInfo const& app, int cx, int cy);
}
