#include "pch.h"
#include "UiSettings.h"

namespace WinUninstaller::Ui
{
    namespace
    {
        constexpr wchar_t kRegPath[] = L"Software\\Leskur\\WinUninstaller";

        DWORD RegGetDword(wchar_t const* name, DWORD fallback)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return fallback;
            }
            DWORD value = fallback;
            DWORD type = 0;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
                type != REG_DWORD)
            {
                value = fallback;
            }
            RegCloseKey(key);
            return value;
        }

        void RegSetDword(wchar_t const* name, DWORD value)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return;
            }
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(key);
        }
    }

    Settings LoadSettings()
    {
        Settings s;
        int lang = static_cast<int>(RegGetDword(L"Language", static_cast<DWORD>(Language::ZhCN)));
        if (lang < 0 || lang > 1) lang = static_cast<int>(Language::ZhCN);
        s.language = static_cast<Language>(lang);
        return s;
    }

    void SaveSettings(Settings const& settings)
    {
        RegSetDword(L"Language", static_cast<DWORD>(settings.language));
    }

    wchar_t const* Tr(Language lang, StrId id)
    {
        struct Entry
        {
            wchar_t const* zh;
            wchar_t const* en;
        };

        static constexpr Entry kTable[] = {
            { L"WinUninstaller 0.0.1", L"WinUninstaller 0.0.1" },
            { L"查看(&V)", L"View(&V)" },
            { L"帮助(&H)", L"Help(&H)" },
            { L"刷新\tF5", L"Refresh\tF5" },
            { L"桌面应用", L"Desktop apps" },
            { L"商店应用", L"Store apps" },
            { L"全部应用", L"All apps" },
            { L"语言", L"Language" },
            { L"中文", L"中文" },
            { L"English", L"English" },
            { L"关于", L"About" },
            { L"主页", L"Home page" },
            { L"反馈问题", L"Report an issue" },
            { L"检查更新", L"Check for updates" },
            { L"搜索应用名称", L"Search by app name" },
            { L"清除", L"Clear" },
            { L"名称", L"Name" },
            { L"大小", L"Size" },
            { L"安装时间", L"Installed" },
            { L"发布者", L"Publisher" },
            { L"应用类型", L"App type" },
            { L"桌面", L"Desktop" },
            { L"商店", L"Store" },
            { L"卸载", L"Uninstall" },
            { L"清理", L"Clean up" },
            { L"打开安装目录", L"Open install folder" },
            { L"打开注册表位置", L"Open registry key" },
            { L"复制名称", L"Copy name" },
            { L"复制安装路径", L"Copy install path" },
            { L"复制注册表路径", L"Copy registry path" },
            { L"刷新列表", L"Refresh list" },
            { L"%u 个应用 占用 %s", L"%u apps, %s" },
            { L" [残留]", L" [leftover]" },
            { L"要清理「%s」勾选的登记项吗？不会删除文件。",
              L"Clean up the selected entries for \"%s\"? Files will not be deleted." },
            { L"(默认)", L"(Default)" },
            { L"[子键]", L"[Subkey]" },
            { L"数据", L"Data" },
            { L"取消", L"Cancel" },
            { L"(二进制，%u 字节)", L"(binary, %u bytes)" },
            { L"清理失败，错误码 %d（0x%08X）。", L"Could not remove the entry. Error %d (0x%08X)." },
            { L"要卸载「%s」吗？", L"Uninstall \"%s\"?" },
            { L"正在卸载 %s…", L"Uninstalling %s…" },
            { L"正在卸载 %s… 若一直不结束，请先关闭正在使用它的程序。",
              L"Uninstalling %s… If this does not finish, close the program that is using it." },
            { L"这个应用正在被其他程序使用，卸载没有完成。请先关闭占用它的程序。",
              L"This app is in use by another program, so it was not removed. Close that program and try again." },
            { L"正在刷新列表…", L"Refreshing the list…" },
            { L"卸载失败，退出码 %d（0x%08X）。", L"Uninstall failed. Exit code %d (0x%08X)." },
            { L"卸载已完成，需要重启计算机后才会完全生效。",
              L"Uninstall finished. Restart the computer to finish removing this app." },
            { L"关于", L"About" },
            { L"WinUninstaller 0.0.1\n轻量 Windows 卸载工具\n\n<A HREF=\"https://github.com/Leskur/win-uninstaller\">https://github.com/Leskur/win-uninstaller</A>\n\n© Leskur",
              L"WinUninstaller 0.0.1\nLightweight Windows uninstaller\n\n<A HREF=\"https://github.com/Leskur/win-uninstaller\">https://github.com/Leskur/win-uninstaller</A>\n\n© Leskur" },
            { L"卸载还在进行。已经启动的卸载程序会继续运行。确定关闭吗？",
              L"Uninstall is still running. Programs that already started will keep running. Close this window?" },
            { L"注册表编辑器正在运行。要定位到该项，需要先关闭它。是否关闭？",
              L"Registry Editor is open. Close it so this key can be selected?" },
            { L"注册表编辑器仍在运行，请先手动关闭后再试。",
              L"Registry Editor is still open. Close it, then try again." },
        };

        static_assert(
            std::size(kTable) == static_cast<size_t>(StrId::RegeditStillOpen) + 1,
            "Tr() table must stay aligned with StrId enum order");

        auto index = static_cast<size_t>(id);
        if (index >= std::size(kTable)) return L"";
        return lang == Language::EnUS ? kTable[index].en : kTable[index].zh;
    }
}
