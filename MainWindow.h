#pragma once

#include "Core/AppInfo.h"
#include "UiSettings.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class MainWindow
{
public:
    explicit MainWindow(HINSTANCE instance);
    int Run(int showCmd);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SearchEditSubclass(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    static LRESULT CALLBACK ListSubclass(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnListCustomDraw(LPNMLVCUSTOMDRAW lvcd);

    bool Create();
    HMENU BuildMenuBar();
    void AttachMenuBar();
    void UpdateMenuState();
    void ApplyLanguage();
    void SetLanguage(::WinUninstaller::Ui::Language language);
    void ShowAbout();
    void OpenUrl(wchar_t const* url);
    void OpenHomePage();
    void Layout();
    void StartScan();
    void StartSizeResolve(std::uint32_t generation, size_t beginIndex);
    void ResetIconCache();
    void RequestVisibleIcons();
    void ApplyLoadedIcons(struct IconBatch* batch);
    void ApplySizeUpdate(struct SizeBatch* update);
    int IconIndexForApp(size_t appIndex, HICON preloaded);
    void ApplyFilter(bool keepPlace = false);
    void SortVisible();
    void UpdateHeaderSortArrows();
    void UpdateSummary();
    void OnUninstall();
    void OnCleanup();
    void ReportUninstallResult(int exitCode, bool cleanup);
    void OnColumnClick(int column);
    void FocusSearch();
    void ClearSearch();
    void SyncSearchQueryFromEdit();
    void UpdateClearButton();
    void DrawHighlightedName(HDC hdc, RECT const& rc, std::wstring const& text, bool selected);
    void ShowItemContextMenu(int x, int y);
    void OpenInstallFolder();
    void OpenRegistryKey();
    void CopyTextToClipboard(std::wstring const& text);
    int SelectedCoreIndex() const;
    ::WinUninstaller::Core::AppInfo const* SelectedApp() const;
    wchar_t const* T(::WinUninstaller::Ui::StrId id) const;

    enum ControlId : int
    {
        IdList = 1006,
        IdStatus = 1007,

        IdUninstall = 2001,
        IdRefresh = 2002,
        IdSearch = 2004,
        IdSearchEscape = 2005,
        IdCategoryDesktop = 2010,
        IdCategoryStore = 2011,
        IdCategoryAll = 2012,
        IdLangZh = 2030,
        IdLangEn = 2031,
        IdAbout = 2040,
        IdHomePage = 2041,
        IdFeedback = 2042,
        IdCheckUpdates = 2043,

        IdCtxUninstall = 1101,
        IdCtxOpenFolder = 1102,
        IdCtxOpenReg = 1103,
        IdCtxCopyName = 1104,
        IdCtxCopyPath = 1106,
        IdCtxCopyReg = 1108,
        IdCtxRefresh = 1107,

        IdSearchEdit = 3001,
        IdSearchClear = 3002,
    };

    enum Msg : UINT
    {
        WM_APP_SCAN_DONE = WM_APP + 1,
        WM_APP_ICONS_DONE = WM_APP + 2,
        WM_APP_SIZE_DONE = WM_APP + 3,
        WM_APP_STATUS = WM_APP + 4,
    };

    struct IconLoadState
    {
        struct Job
        {
            std::uint32_t generation{ 0 };
            size_t index{ 0 };
            int cx{ 16 };
            int cy{ 16 };
            ::WinUninstaller::Core::AppInfo app;
        };

        std::mutex mu;
        std::vector<Job> queue;
        bool running{ false };
        std::atomic<bool> stop{ false };
    };

    static constexpr UINT_PTR kSearchSubclassId = 1;
    static constexpr UINT_PTR kListSubclassId = 2;
    static constexpr UINT_PTR kUninstallTimerId = 1;

    void SetStatusText(std::wstring const& text);

    HINSTANCE m_instance{ nullptr };
    HWND m_hwnd{ nullptr };
    HWND m_search{ nullptr };
    HWND m_searchClear{ nullptr };
    HWND m_searchTip{ nullptr };
    HWND m_list{ nullptr };
    HWND m_status{ nullptr };
    HFONT m_font{ nullptr };
    HACCEL m_accel{ nullptr };
    HMENU m_menu{ nullptr };
    HIMAGELIST m_imageList{ nullptr };
    int m_defaultIconIndex{ 0 };
    std::vector<int> m_appIconIndex;
    std::vector<std::uint8_t> m_iconState;
    std::shared_ptr<IconLoadState> m_iconLoad{ std::make_shared<IconLoadState>() };
    std::unordered_map<std::wstring, int> m_iconCache;

    std::vector<::WinUninstaller::Core::AppInfo> m_apps;
    std::vector<size_t> m_visible;
    std::atomic_uint32_t m_scanGeneration{ 0 };
    bool m_busy{ false };
    int m_dpi{ 96 };
    int m_sortColumn{ 0 };
    bool m_sortAscending{ true };
    int m_category{ 2 };
    std::wstring m_searchQuery;
    std::wstring m_pendingUninstallName;
    ::WinUninstaller::Ui::Settings m_settings{};
};
