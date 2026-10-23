#include "pch.h"
#include "MainWindow.h"
#include "Core/Uninstaller.h"

#include <roapi.h>

#pragma comment(lib, "runtimeobject.lib")

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int showCmd)
{
    // Needed for PackageManager MSIX enumeration.
    RoInitialize(RO_INIT_MULTITHREADED);

    MainWindow window(instance);
    int code = window.Run(showCmd);
    ::WinUninstaller::Core::WaitForWinrtWorkers();

    RoUninitialize();
    return code;
}
