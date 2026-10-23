#include "pch.h"
#include "MainWindow.h"
#include "Core/AppScanner.h"
#include "Core/Format.h"
#include "Core/Icons.h"
#include "Core/Uninstaller.h"

#include <CommCtrl.h>

#ifdef GetCurrentTime
    #undef GetCurrentTime
#endif
#include <winrt/base.h>

namespace
{
    constexpr wchar_t kClassName[] = L"WinUninstaller.MainWindow";
    constexpr int kPad = 8;
    constexpr int kSearchH = 28;
    constexpr int kSearchW = 260;
    constexpr COLORREF kListAlt = RGB(245, 247, 250);
    constexpr COLORREF kMatch = RGB(200, 0, 0);
    constexpr COLORREF kMatchSel = RGB(255, 180, 180);
    constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Leskur.WinUninstaller";

    void ActivateExistingWindow(HWND hwnd)
    {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
        DWORD currentThread = GetCurrentThreadId();
        bool attached = targetThread != 0 && targetThread != currentThread
            && AttachThreadInput(currentThread, targetThread, TRUE);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (attached) AttachThreadInput(currentThread, targetThread, FALSE);
    }

    bool AcquireSingleInstance()
    {
        SetLastError(0);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
        if (!mutex) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            static HANDLE held = nullptr;
            held = mutex;
            return true;
        }

        CloseHandle(mutex);
        HWND existing = nullptr;
        for (int i = 0; i < 20 && !existing; ++i)
        {
            existing = FindWindowW(kClassName, nullptr);
            if (!existing) Sleep(50);
        }
        if (existing) ActivateExistingWindow(existing);
        return false;
    }

    struct ScanDonePayload
    {
        std::vector<::WinUninstaller::Core::AppInfo> apps;
        bool append{ false };
        bool complete{ true };
        bool reportUninstall{ false };
        bool cleanup{ false };
        int exitCode{ 0 };
    };

    std::wstring ToLowerCopy(std::wstring s)
    {
        for (auto& c : s) c = towlower(c);
        return s;
    }

    struct MessageBoxCenter
    {
        static HWND& Owner()
        {
            thread_local HWND owner = nullptr;
            return owner;
        }

        static HHOOK& Hook()
        {
            thread_local HHOOK hook = nullptr;
            return hook;
        }

        static LRESULT CALLBACK Proc(int code, WPARAM wParam, LPARAM lParam)
        {
            if (code == HCBT_ACTIVATE && Owner())
            {
                HWND dialog = reinterpret_cast<HWND>(wParam);
                wchar_t className[32]{};
                GetClassNameW(dialog, className, 32);
                if (wcscmp(className, L"#32770") == 0)
                {
                    RECT ownerRc{};
                    RECT dialogRc{};
                    GetWindowRect(Owner(), &ownerRc);
                    GetWindowRect(dialog, &dialogRc);
                    int width = dialogRc.right - dialogRc.left;
                    int height = dialogRc.bottom - dialogRc.top;
                    int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
                    int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;

                    MONITORINFO monitor{ sizeof(monitor) };
                    if (GetMonitorInfoW(MonitorFromWindow(Owner(), MONITOR_DEFAULTTONEAREST), &monitor))
                    {
                        RECT const& work = monitor.rcWork;
                        if (x < work.left) x = work.left;
                        if (y < work.top) y = work.top;
                        if (x + width > work.right) x = work.right - width;
                        if (y + height > work.bottom) y = work.bottom - height;
                    }

                    SetWindowPos(
                        dialog, nullptr, x, y, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    HHOOK installed = Hook();
                    UnhookWindowsHookEx(installed);
                    Hook() = nullptr;
                    Owner() = nullptr;
                    return CallNextHookEx(installed, code, wParam, lParam);
                }
            }
            return CallNextHookEx(Hook(), code, wParam, lParam);
        }
    };

    int MessageBoxCentered(HWND owner, wchar_t const* text, wchar_t const* caption, UINT type)
    {
        MessageBoxCenter::Owner() = owner;
        MessageBoxCenter::Hook() = SetWindowsHookExW(
            WH_CBT, MessageBoxCenter::Proc, nullptr, GetCurrentThreadId());
        int result = MessageBoxW(owner, text, caption, type);
        if (MessageBoxCenter::Hook())
        {
            UnhookWindowsHookEx(MessageBoxCenter::Hook());
            MessageBoxCenter::Hook() = nullptr;
            MessageBoxCenter::Owner() = nullptr;
        }
        return result;
    }

    HRESULT CALLBACK AboutLinkCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR)
    {
        if (msg == TDN_HYPERLINK_CLICKED)
        {
            ShellExecuteW(
                hwnd, L"open", reinterpret_cast<wchar_t const*>(lParam),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    }

    void CenterOnWorkArea(HWND hwnd)
    {
        RECT windowRc{};
        if (!GetWindowRect(hwnd, &windowRc)) return;
        int width = windowRc.right - windowRc.left;
        int height = windowRc.bottom - windowRc.top;

        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return;
        RECT const& work = info.rcWork;
        int x = work.left + ((work.right - work.left) - width) / 2;
        int y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

MainWindow::MainWindow(HINSTANCE instance)
    : m_instance(instance)
{
    m_settings = ::WinUninstaller::Ui::LoadSettings();
}

wchar_t const* MainWindow::T(::WinUninstaller::Ui::StrId id) const
{
    return ::WinUninstaller::Ui::Tr(m_settings.language, id);
}

HMENU MainWindow::BuildMenuBar()
{
    using ::WinUninstaller::Ui::StrId;

    HMENU bar = CreateMenu();
    HMENU view = CreatePopupMenu();
    HMENU lang = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    if (!bar || !view || !lang || !help)
    {
        if (lang) DestroyMenu(lang);
        if (view) DestroyMenu(view);
        if (help) DestroyMenu(help);
        if (bar) DestroyMenu(bar);
        return nullptr;
    }

    AppendMenuW(view, MF_STRING, IdRefresh, T(StrId::Refresh));
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdCategoryAll, T(StrId::CategoryAll));
    AppendMenuW(view, MF_STRING, IdCategoryDesktop, T(StrId::CategoryDesktop));
    AppendMenuW(view, MF_STRING, IdCategoryStore, T(StrId::CategoryStore));
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(lang, MF_STRING, IdLangZh, T(StrId::LangZh));
    AppendMenuW(lang, MF_STRING, IdLangEn, T(StrId::LangEn));
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), T(StrId::Language));

    AppendMenuW(help, MF_STRING, IdHomePage, T(StrId::HomePage));
    AppendMenuW(help, MF_STRING, IdFeedback, T(StrId::Feedback));
    AppendMenuW(help, MF_STRING, IdCheckUpdates, T(StrId::CheckUpdates));
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IdAbout, T(StrId::About));

    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), T(StrId::MenuView));
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), T(StrId::MenuHelp));
    return bar;
}

void MainWindow::AttachMenuBar()
{
    HMENU old = GetMenu(m_hwnd);
    m_menu = BuildMenuBar();
    SetMenu(m_hwnd, m_menu);
    DrawMenuBar(m_hwnd);
    if (old) DestroyMenu(old);
}

bool MainWindow::Create()
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    m_menu = BuildMenuBar();
    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        T(::WinUninstaller::Ui::StrId::AppTitle),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1024,
        768,
        nullptr,
        m_menu,
        m_instance,
        this);
    if (!m_hwnd) return false;
    CenterOnWorkArea(m_hwnd);

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'F', static_cast<WORD>(IdSearch) },
        { FVIRTKEY, VK_F5, static_cast<WORD>(IdRefresh) },
        { FVIRTKEY, VK_ESCAPE, static_cast<WORD>(IdSearchEscape) },
    };
    m_accel = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));
    return true;
}

int MainWindow::Run(int showCmd)
{
    if (!AcquireSingleInstance()) return 0;
    if (!Create()) return 1;
    ShowWindow(m_hwnd, showCmd);
    UpdateWindow(m_hwnd);
    StartScan();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (m_accel && TranslateAcceleratorW(m_hwnd, m_accel, &msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SearchEditSubclass(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* self = reinterpret_cast<MainWindow*>(refData);
    if (self && msg == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN)
        {
            SetFocus(self->m_list);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static int ListHorizontalScrollPixels(HWND list, int wheelDelta)
{
    UINT chars = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);

    int pixelsPerNotch = 1;
    if (chars == WHEEL_PAGESCROLL)
    {
        RECT rc{};
        GetClientRect(list, &rc);
        pixelsPerNotch = rc.right - rc.left;
    }
    else
    {
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
        HDC hdc = GetDC(list);
        HFONT old = font ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        if (old) SelectObject(hdc, old);
        ReleaseDC(list, hdc);
        int charW = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
        pixelsPerNotch = static_cast<int>(chars) * charW;
    }
    if (pixelsPerNotch < 1) pixelsPerNotch = 1;

    // Keep the fractional notch so a high-resolution wheel still moves.
    static int remainder = 0;
    remainder += -wheelDelta * pixelsPerNotch;
    int dx = remainder / WHEEL_DELTA;
    remainder %= WHEEL_DELTA;
    return dx;
}

LRESULT CALLBACK MainWindow::ListSubclass(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* self = reinterpret_cast<MainWindow*>(refData);
    if (self && msg == WM_KEYDOWN && wParam == VK_DELETE)
    {
        SendMessageW(self->m_hwnd, WM_COMMAND, MAKEWPARAM(IdUninstall, 0), 0);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT))
    {
        int dx = ListHorizontalScrollPixels(hwnd, GET_WHEEL_DELTA_WPARAM(wParam));
        if (dx != 0)
        {
            ListView_Scroll(hwnd, dx, 0);
        }
        return 0;
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (self && (msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN || msg == WM_VSCROLL))
    {
        self->RequestVisibleIcons();
    }
    return result;
}

void MainWindow::FocusSearch()
{
    if (!m_search) return;
    SetFocus(m_search);
    SendMessageW(m_search, EM_SETSEL, 0, -1);
}

void MainWindow::ClearSearch()
{
    if (!m_search) return;
    SetWindowTextW(m_search, L"");
    m_searchQuery.clear();
    ApplyFilter();
    UpdateClearButton();
}

void MainWindow::SyncSearchQueryFromEdit()
{
    if (!m_search) return;
    int len = GetWindowTextLengthW(m_search);
    m_searchQuery.assign(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(m_search, m_searchQuery.data(), len + 1);
    m_searchQuery.resize(wcslen(m_searchQuery.c_str()));
    ApplyFilter();
    UpdateClearButton();
}

void MainWindow::UpdateClearButton()
{
    if (!m_search || !m_searchClear) return;

    bool show = !m_searchQuery.empty();
    RECT rc{};
    GetWindowRect(m_search, &rc);
    MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&rc), 2);
    int edge = GetSystemMetrics(SM_CXEDGE);
    int btn = (rc.bottom - rc.top) - edge * 2 - 2;
    if (btn < 16) btn = 16;
    int x = rc.right - edge - btn - 1;
    int y = rc.top + ((rc.bottom - rc.top) - btn) / 2;
    MoveWindow(m_searchClear, x, y, btn, btn, TRUE);
    SetWindowPos(m_searchClear, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(m_searchClear, show ? SW_SHOW : SW_HIDE);
    SendMessageW(m_search, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, show ? btn + 4 : 0));
}

void MainWindow::UpdateMenuState()
{
    HMENU bar = GetMenu(m_hwnd);
    if (!bar) return;

    EnableMenuItem(bar, IdRefresh, MF_BYCOMMAND | (m_busy ? MF_GRAYED : MF_ENABLED));

    CheckMenuItem(bar, IdCategoryDesktop, MF_BYCOMMAND | (m_category == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IdCategoryStore, MF_BYCOMMAND | (m_category == 1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IdCategoryAll, MF_BYCOMMAND | (m_category == 2 ? MF_CHECKED : MF_UNCHECKED));

    using Lang = ::WinUninstaller::Ui::Language;
    CheckMenuItem(bar, IdLangZh, MF_BYCOMMAND | (m_settings.language == Lang::ZhCN ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IdLangEn, MF_BYCOMMAND | (m_settings.language == Lang::EnUS ? MF_CHECKED : MF_UNCHECKED));
}

void MainWindow::ApplyLanguage()
{
    using ::WinUninstaller::Ui::StrId;
    if (!m_hwnd) return;

    SetWindowTextW(m_hwnd, T(StrId::AppTitle));
    AttachMenuBar();
    UpdateMenuState();

    if (m_search)
    {
        SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(StrId::SearchCue)));
    }
    if (m_searchTip && m_searchClear)
    {
        TOOLINFOW tip{};
        tip.cbSize = sizeof(tip);
        tip.hwnd = m_hwnd;
        tip.uId = reinterpret_cast<UINT_PTR>(m_searchClear);
        tip.lpszText = const_cast<wchar_t*>(T(StrId::Clear));
        SendMessageW(m_searchTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tip));
    }

    if (m_list)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        auto setCol = [&](int index, StrId id) {
            col.pszText = const_cast<wchar_t*>(T(id));
            ListView_SetColumn(m_list, index, &col);
        };
        setCol(0, StrId::ColName);
        setCol(1, StrId::ColSize);
        setCol(2, StrId::ColType);
        setCol(3, StrId::ColDate);
        setCol(4, StrId::ColPublisher);
    }

    UpdateSummary();
    if (m_list) ApplyFilter();
    Layout();
}

void MainWindow::SetLanguage(::WinUninstaller::Ui::Language language)
{
    m_settings.language = language;
    ::WinUninstaller::Ui::SaveSettings(m_settings);
    ApplyLanguage();
}

void MainWindow::ShowAbout()
{
    using ::WinUninstaller::Ui::StrId;
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = m_hwnd;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = T(StrId::AboutTitle);
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszContent = T(StrId::AboutBody);
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pfCallback = AboutLinkCallback;

    MessageBoxCenter::Owner() = m_hwnd;
    MessageBoxCenter::Hook() = SetWindowsHookExW(
        WH_CBT, MessageBoxCenter::Proc, nullptr, GetCurrentThreadId());
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    if (MessageBoxCenter::Hook())
    {
        UnhookWindowsHookEx(MessageBoxCenter::Hook());
        MessageBoxCenter::Hook() = nullptr;
        MessageBoxCenter::Owner() = nullptr;
    }
}

void MainWindow::OpenUrl(wchar_t const* url)
{
    ShellExecuteW(m_hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::OpenHomePage()
{
    OpenUrl(L"https://github.com/Leskur/win-uninstaller");
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        using ::WinUninstaller::Ui::StrId;
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        m_font = CreateFontIndirectW(&ncm.lfMessageFont);
        m_dpi = static_cast<int>(GetDpiForWindow(m_hwnd));
        if (m_dpi <= 0) m_dpi = 96;

        m_search = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_CLIPSIBLINGS,
            0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(IdSearchEdit), m_instance, nullptr);
        SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(StrId::SearchCue)));
        SetWindowSubclass(m_search, SearchEditSubclass, kSearchSubclassId, reinterpret_cast<DWORD_PTR>(this));

        m_searchClear = CreateWindowExW(
            0, L"BUTTON", L"\u00D7",
            WS_CHILD | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_CENTER | BS_FLAT,
            0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(IdSearchClear), m_instance, nullptr);
        m_searchTip = CreateWindowExW(
            0, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP,
            0, 0, 0, 0, m_hwnd, nullptr, m_instance, nullptr);
        if (m_searchTip)
        {
            TOOLINFOW tip{};
            tip.cbSize = sizeof(tip);
            tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tip.hwnd = m_hwnd;
            tip.uId = reinterpret_cast<UINT_PTR>(m_searchClear);
            tip.lpszText = const_cast<wchar_t*>(T(StrId::Clear));
            SendMessageW(m_searchTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
        }

        m_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
            0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(IdList), m_instance, nullptr);
        ListView_SetExtendedListViewStyle(
            m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowSubclass(m_list, ListSubclass, kListSubclassId, reinterpret_cast<DWORD_PTR>(this));

        int cx = GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(m_dpi));
        int cy = GetSystemMetricsForDpi(SM_CYSMICON, static_cast<UINT>(m_dpi));
        m_imageList = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 64, 64);
        HICON defaultIcon = LoadIconW(nullptr, IDI_APPLICATION);
        m_defaultIconIndex = ImageList_AddIcon(m_imageList, defaultIcon);
        ListView_SetImageList(m_list, m_imageList, LVSIL_SMALL);

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        col.pszText = const_cast<wchar_t*>(T(StrId::ColName));
        col.cx = 300;
        col.iSubItem = 0;
        ListView_InsertColumn(m_list, 0, &col);
        col.fmt = LVCFMT_RIGHT;
        col.pszText = const_cast<wchar_t*>(T(StrId::ColSize));
        col.cx = 90;
        col.iSubItem = 1;
        ListView_InsertColumn(m_list, 1, &col);
        col.fmt = LVCFMT_LEFT;
        col.pszText = const_cast<wchar_t*>(T(StrId::ColType));
        col.cx = 100;
        col.iSubItem = 2;
        ListView_InsertColumn(m_list, 2, &col);
        col.pszText = const_cast<wchar_t*>(T(StrId::ColDate));
        col.cx = 110;
        col.iSubItem = 3;
        ListView_InsertColumn(m_list, 3, &col);
        col.pszText = const_cast<wchar_t*>(T(StrId::ColPublisher));
        col.cx = 220;
        col.iSubItem = 4;
        ListView_InsertColumn(m_list, 4, &col);

        m_status = CreateWindowExW(
            0, STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(IdStatus), m_instance, nullptr);

        SendMessageW(m_search, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        SendMessageW(m_searchClear, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        SendMessageW(m_status, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        Layout();
        UpdateMenuState();
        return 0;
    }
    case WM_SIZE:
        Layout();
        RequestVisibleIcons();
        return 0;
    case WM_DPICHANGED:
    {
        int newDpi = HIWORD(wParam);
        if (newDpi <= 0) newDpi = m_dpi > 0 ? m_dpi : 96;
        int oldDpi = m_dpi > 0 ? m_dpi : newDpi;
        NONCLIENTMETRICSW dpiMetrics{ sizeof(dpiMetrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(dpiMetrics), &dpiMetrics, 0, static_cast<UINT>(newDpi)))
        {
            HFONT font = CreateFontIndirectW(&dpiMetrics.lfMessageFont);
            if (font)
            {
                if (m_font) DeleteObject(m_font);
                m_font = font;
                if (m_search) SendMessageW(m_search, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                if (m_searchClear) SendMessageW(m_searchClear, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                if (m_list) SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                if (m_status) SendMessageW(m_status, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
            }
        }
        HWND header = m_list ? ListView_GetHeader(m_list) : nullptr;
        if (header && oldDpi != newDpi)
        {
            int columns = Header_GetItemCount(header);
            for (int i = 0; i < columns; ++i)
            {
                int width = ListView_GetColumnWidth(m_list, i);
                ListView_SetColumnWidth(m_list, i, MulDiv(width, newDpi, oldDpi));
            }
        }
        m_dpi = newDpi;
        if (m_list)
        {
            int cx = GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(newDpi));
            int cy = GetSystemMetricsForDpi(SM_CYSMICON, static_cast<UINT>(newDpi));
            if (cx <= 0) cx = 16;
            if (cy <= 0) cy = 16;
            HIMAGELIST fresh = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 64, 64);
            if (fresh)
            {
                HIMAGELIST previous = ListView_SetImageList(m_list, fresh, LVSIL_SMALL);
                if (previous) ImageList_Destroy(previous);
                m_imageList = fresh;
                ResetIconCache();
            }
        }
        auto* suggested = reinterpret_cast<RECT*>(lParam);
        if (suggested)
        {
            SetWindowPos(
                m_hwnd, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Layout();
        RequestVisibleIcons();
        return 0;
    }
    case WM_CLOSE:
        if (m_busy && !m_pendingUninstallName.empty())
        {
            using ::WinUninstaller::Ui::StrId;
            int answer = MessageBoxCentered(
                m_hwnd,
                T(StrId::CloseWhileUninstalling),
                T(StrId::AppTitle),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return 0;
        }
        ::WinUninstaller::Core::RequestWorkerShutdown();
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    case WM_INITMENUPOPUP:
        UpdateMenuState();
        return 0;
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IdSearchEdit && code == EN_CHANGE)
        {
            SyncSearchQueryFromEdit();
            return 0;
        }
        switch (id)
        {
        case IdUninstall:
        case IdCtxUninstall:
            if (auto* selected = SelectedApp(); selected && selected->IsOrphaned)
                OnCleanup();
            else
                OnUninstall();
            return 0;
        case IdRefresh:
        case IdCtxRefresh:
            StartScan();
            return 0;
        case IdSearch:
            FocusSearch();
            return 0;
        case IdSearchEscape:
            if (!m_searchQuery.empty())
            {
                ClearSearch();
            }
            else if (GetFocus() == m_search)
            {
                SetFocus(m_list);
            }
            return 0;
        case IdSearchClear:
            ClearSearch();
            FocusSearch();
            return 0;
        case IdCategoryDesktop:
            m_category = 0;
            ApplyFilter();
            return 0;
        case IdCategoryStore:
            m_category = 1;
            ApplyFilter();
            return 0;
        case IdCategoryAll:
            m_category = 2;
            ApplyFilter();
            return 0;
        case IdLangZh:
            SetLanguage(::WinUninstaller::Ui::Language::ZhCN);
            return 0;
        case IdLangEn:
            SetLanguage(::WinUninstaller::Ui::Language::EnUS);
            return 0;
        case IdAbout:
            ShowAbout();
            return 0;
        case IdHomePage:
            OpenHomePage();
            return 0;
        case IdFeedback:
            OpenUrl(L"https://github.com/Leskur/win-uninstaller/issues");
            return 0;
        case IdCheckUpdates:
            OpenUrl(L"https://github.com/Leskur/win-uninstaller/releases");
            return 0;
        case IdCtxOpenFolder:
            OpenInstallFolder();
            return 0;
        case IdCtxOpenReg:
            OpenRegistryKey();
            return 0;
        case IdCtxCopyName:
            if (auto* app = SelectedApp()) CopyTextToClipboard(app->DisplayName);
            return 0;
        case IdCtxCopyPath:
            if (auto* app = SelectedApp()) CopyTextToClipboard(app->InstallLocation);
            return 0;
        case IdCtxCopyReg:
            if (auto* app = SelectedApp()) CopyTextToClipboard(app->RegistryKeyPath);
            return 0;
        default:
            break;
        }
        return 0;
    }
    case WM_NOTIFY:
    {
        auto hdr = reinterpret_cast<LPNMHDR>(lParam);
        if (hdr->idFrom == IdList)
        {
            if (hdr->code == LVN_ITEMCHANGED)
            {
                UpdateMenuState();
            }
            else if (hdr->code == LVN_COLUMNCLICK)
            {
                auto click = reinterpret_cast<LPNMLISTVIEW>(lParam);
                OnColumnClick(click->iSubItem);
            }
            else if (hdr->code == LVN_ENDSCROLL)
            {
                RequestVisibleIcons();
            }
            else if (hdr->code == NM_CUSTOMDRAW)
            {
                return OnListCustomDraw(reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam));
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
    {
        if (reinterpret_cast<HWND>(wParam) == m_list || reinterpret_cast<HWND>(wParam) == m_hwnd)
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (x == -1 && y == -1)
            {
                RECT rc{};
                GetWindowRect(m_list, &rc);
                x = rc.left + 20;
                y = rc.top + 40;
            }
            ShowItemContextMenu(x, y);
            return 0;
        }
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
    case WM_APP_SCAN_DONE:
    {
        auto* payload = reinterpret_cast<ScanDonePayload*>(lParam);
        auto generation = static_cast<std::uint32_t>(wParam);
        if (!payload || generation != m_scanGeneration)
        {
            delete payload;
            return 0;
        }
        bool append = payload->append;
        bool complete = payload->complete;
        bool reportUninstall = payload->reportUninstall;
        bool cleanup = payload->cleanup;
        int exitCode = payload->exitCode;
        if (append)
        {
            size_t begin = m_apps.size();
            m_apps.insert(
                m_apps.end(),
                std::make_move_iterator(payload->apps.begin()),
                std::make_move_iterator(payload->apps.end()));
            delete payload;
            if (m_appIconIndex.size() < m_apps.size())
            {
                m_appIconIndex.resize(m_apps.size(), m_defaultIconIndex);
            }
            if (m_iconState.size() < m_apps.size())
            {
                m_iconState.resize(m_apps.size(), 0);
            }
            ApplyFilter();
            StartSizeResolve(generation, begin);
        }
        else
        {
            m_apps = std::move(payload->apps);
            delete payload;
            ResetIconCache();
            ApplyFilter();
            StartSizeResolve(generation, 0);
        }
        KillTimer(m_hwnd, kUninstallTimerId);
        if (complete)
        {
            m_busy = false;
            m_pendingUninstallName.clear();
            UpdateMenuState();
            if (reportUninstall) ReportUninstallResult(exitCode, cleanup);
        }
        return 0;
    }
    case WM_APP_ICONS_DONE:
    {
        ApplyLoadedIcons(reinterpret_cast<IconBatch*>(lParam));
        return 0;
    }
    case WM_APP_SIZE_DONE:
    {
        ApplySizeUpdate(reinterpret_cast<SizeBatch*>(lParam));
        return 0;
    }
    case WM_APP_STATUS:
    {
        auto* text = reinterpret_cast<std::wstring*>(lParam);
        if (text)
        {
            SetStatusText(*text);
            delete text;
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kUninstallTimerId && m_busy && !m_pendingUninstallName.empty())
        {
            KillTimer(m_hwnd, kUninstallTimerId);
            int needed = _scwprintf(T(::WinUninstaller::Ui::StrId::UninstallWaiting), m_pendingUninstallName.c_str());
            if (needed > 0)
            {
                std::wstring text(static_cast<size_t>(needed) + 1, L'\0');
                swprintf_s(
                    text.data(), text.size(),
                    T(::WinUninstaller::Ui::StrId::UninstallWaiting),
                    m_pendingUninstallName.c_str());
                text.resize(static_cast<size_t>(needed));
                SetStatusText(text);
            }
        }
        return 0;
    case WM_DESTROY:
        ::WinUninstaller::Core::RequestWorkerShutdown();
        KillTimer(m_hwnd, kUninstallTimerId);
        ++m_scanGeneration;
        if (m_iconLoad)
        {
            std::lock_guard<std::mutex> lock(m_iconLoad->mu);
            m_iconLoad->stop = true;
            m_iconLoad->queue.clear();
        }
        if (m_searchTip)
        {
            DestroyWindow(m_searchTip);
            m_searchTip = nullptr;
        }
        if (m_search)
        {
            RemoveWindowSubclass(m_search, SearchEditSubclass, kSearchSubclassId);
        }
        if (m_list)
        {
            RemoveWindowSubclass(m_list, ListSubclass, kListSubclassId);
        }
        if (m_accel)
        {
            DestroyAcceleratorTable(m_accel);
            m_accel = nullptr;
        }
        if (m_imageList)
        {
            ListView_SetImageList(m_list, nullptr, LVSIL_SMALL);
            ImageList_Destroy(m_imageList);
            m_imageList = nullptr;
        }
        if (m_font)
        {
            DeleteObject(m_font);
            m_font = nullptr;
        }
        m_menu = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}

void MainWindow::Layout()
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (m_status) SendMessageW(m_status, WM_SIZE, 0, 0);
    RECT statusRc{};
    int statusH = 0;
    if (m_status && GetWindowRect(m_status, &statusRc))
    {
        statusH = statusRc.bottom - statusRc.top;
    }
    int y = kPad;

    int searchW = kSearchW;
    if (searchW > w - kPad * 2) searchW = w - kPad * 2;
    if (searchW < 120) searchW = 120;
    int searchX = w - kPad - searchW;
    if (searchX < kPad) searchX = kPad;
    MoveWindow(m_search, searchX, y, searchW, kSearchH, TRUE);
    UpdateClearButton();
    y += kSearchH + kPad;

    int listH = h - y - statusH;
    if (listH < 80) listH = 80;
    MoveWindow(m_list, kPad, y, w - kPad * 2, listH, TRUE);
}

void MainWindow::StartScan()
{
    if (m_busy) return;
    m_busy = true;
    UpdateMenuState();
    ListView_DeleteAllItems(m_list);
    m_visible.clear();
    UpdateSummary();

    auto generation = ++m_scanGeneration;
    HWND hwnd = m_hwnd;
    std::thread([hwnd, generation]() {
        ::WinUninstaller::Core::WinrtLease lease;
        try
        {
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }
        catch (...)
        {
        }

        auto postOrDelete = [hwnd, generation](ScanDonePayload* payload) {
            if (!PostMessageW(hwnd, WM_APP_SCAN_DONE, generation, reinterpret_cast<LPARAM>(payload)))
            {
                delete payload;
                return false;
            }
            return true;
        };

        bool postedRegistry = false;
        try
        {
            auto* registry = new ScanDonePayload();
            registry->complete = false;
            registry->apps = ::WinUninstaller::Core::EnumerateRegistryApplications();
            if (::WinUninstaller::Core::WorkerShutdownRequested())
            {
                delete registry;
                return;
            }
            if (!postOrDelete(registry)) return;
            postedRegistry = true;

            auto* store = new ScanDonePayload();
            store->append = true;
            store->complete = true;
            if (!::WinUninstaller::Core::WorkerShutdownRequested())
            {
                store->apps = ::WinUninstaller::Core::EnumerateMsixApplications();
            }
            postOrDelete(store);
        }
        catch (...)
        {
            auto* failed = new ScanDonePayload();
            if (postedRegistry)
            {
                failed->append = true;
                failed->complete = true;
            }
            postOrDelete(failed);
        }
    }).detach();
}

void MainWindow::OnColumnClick(int column)
{
    if (column < 0 || column > 4) return;
    if (m_sortColumn == column)
    {
        m_sortAscending = !m_sortAscending;
    }
    else
    {
        m_sortColumn = column;
        // Size defaults to descending (large first); others ascending.
        m_sortAscending = (column != 1);
    }
    ApplyFilter();
}

void MainWindow::SortVisible()
{
    auto cmpText = [&](std::wstring const& a, std::wstring const& b) {
        int r = CompareStringEx(
            LOCALE_NAME_USER_DEFAULT,
            LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
            a.c_str(), static_cast<int>(a.size()),
            b.c_str(), static_cast<int>(b.size()),
            nullptr, nullptr, 0);
        if (r == 0) r = CSTR_EQUAL;
        return m_sortAscending ? (r == CSTR_LESS_THAN) : (r == CSTR_GREATER_THAN);
    };

    std::sort(m_visible.begin(), m_visible.end(), [&](size_t ia, size_t ib) {
        auto const& a = m_apps[ia];
        auto const& b = m_apps[ib];
        switch (m_sortColumn)
        {
        case 1: // size
            if (a.EstimatedSizeBytes == b.EstimatedSizeBytes)
                return cmpText(a.DisplayName, b.DisplayName);
            return m_sortAscending
                ? (a.EstimatedSizeBytes < b.EstimatedSizeBytes)
                : (a.EstimatedSizeBytes > b.EstimatedSizeBytes);
        case 2: // desktop before store when ascending
        {
            bool aStore = a.SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
            bool bStore = b.SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
            if (aStore != bStore)
            {
                return m_sortAscending ? (aStore < bStore) : (aStore > bStore);
            }
            return cmpText(a.DisplayName, b.DisplayName);
        }
        case 3: // install date
            return cmpText(a.InstallDate, b.InstallDate);
        case 4: // publisher
            return cmpText(a.Publisher, b.Publisher);
        case 0:
        default:
            return cmpText(a.DisplayName, b.DisplayName);
        }
    });
}

struct IconBatch
{
    struct Item
    {
        std::uint32_t generation{ 0 };
        size_t index{ 0 };
        HICON icon{ nullptr };
        int cx{ 0 };
    };
    std::vector<Item> items;
};

struct SizeBatch
{
    std::uint32_t generation{ 0 };
    size_t index{ 0 };
    std::int64_t bytes{ 0 };
    bool finished{ false };
    bool keepExisting{ false };
};

void MainWindow::ResetIconCache()
{
    if (m_iconLoad)
    {
        std::lock_guard<std::mutex> lock(m_iconLoad->mu);
        m_iconLoad->queue.clear();
    }
    m_iconCache.clear();
    if (m_imageList)
    {
        ImageList_Remove(m_imageList, -1);
        HICON defaultIcon = LoadIconW(nullptr, IDI_APPLICATION);
        m_defaultIconIndex = ImageList_AddIcon(m_imageList, defaultIcon);
        if (m_defaultIconIndex < 0) m_defaultIconIndex = 0;
    }
    m_appIconIndex.assign(m_apps.size(), m_defaultIconIndex);
    m_iconState.assign(m_apps.size(), 0);
}

void MainWindow::StartSizeResolve(std::uint32_t generation, size_t beginIndex)
{
    struct Pending
    {
        size_t index;
        std::wstring path;
        bool cacheSize{ false };
    };
    std::vector<Pending> pending;
    for (size_t i = beginIndex; i < m_apps.size(); ++i)
    {
        if (!m_apps[i].SizePending) continue;
        bool cacheSize = m_apps[i].SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
        pending.push_back({ i, m_apps[i].InstallLocation, cacheSize });
    }
    if (pending.empty()) return;

    HWND hwnd = m_hwnd;
    std::thread([hwnd, generation, pending = std::move(pending)]() {
        bool stored = false;
        for (size_t n = 0; n < pending.size(); ++n)
        {
            if (::WinUninstaller::Core::WorkerShutdownRequested()) break;
            std::int64_t bytes = 0;
            bool measured = ::WinUninstaller::Core::Format::TryMeasureDirectorySize(pending[n].path, bytes);
            if (measured && pending[n].cacheSize)
            {
                ::WinUninstaller::Core::Format::StoreCachedDirectorySize(pending[n].path, bytes);
                stored = true;
            }
            auto* payload = new SizeBatch{};
            payload->generation = generation;
            payload->index = pending[n].index;
            payload->bytes = measured ? bytes : 0;
            payload->keepExisting = !measured;
            payload->finished = (n + 1 == pending.size());
            if (!PostMessageW(hwnd, MainWindow::WM_APP_SIZE_DONE, generation, reinterpret_cast<LPARAM>(payload)))
            {
                delete payload;
            }
        }
        if (stored) ::WinUninstaller::Core::Format::FlushDirectorySizeCache();
    }).detach();
}

void MainWindow::ApplySizeUpdate(SizeBatch* update)
{
    if (!update) return;
    std::uint32_t generation = update->generation;
    size_t index = update->index;
    std::int64_t bytes = update->bytes;
    bool finished = update->finished;
    bool keepExisting = update->keepExisting;
    delete update;

    if (generation != m_scanGeneration) return;
    if (index < m_apps.size() && m_apps[index].SizePending)
    {
        auto& app = m_apps[index];
        app.SizePending = false;
        if (!keepExisting)
        {
            if (app.IsOrphaned)
            {
                app.EstimatedSizeBytes = 0;
                app.FormattedSize = L"-";
            }
            else
            {
                app.EstimatedSizeBytes = bytes;
                app.FormattedSize = ::WinUninstaller::Core::Format::FormatBytes(bytes);
            }
            if (m_list)
            {
                for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
                {
                    if (m_visible[static_cast<size_t>(row)] != index) continue;
                    ListView_SetItemText(m_list, row, 1, const_cast<wchar_t*>(app.FormattedSize.c_str()));
                    break;
                }
            }
            UpdateSummary();
        }
    }
    if (finished && m_sortColumn == 1) ApplyFilter(true);
}

void MainWindow::RequestVisibleIcons()
{
    if (!m_list || m_apps.empty() || !m_iconLoad || m_iconLoad->stop) return;
    if (m_iconState.size() != m_apps.size()) m_iconState.assign(m_apps.size(), 0);

    int top = ListView_GetTopIndex(m_list);
    int page = ListView_GetCountPerPage(m_list);
    if (top < 0) top = 0;
    if (page < 1) page = 40;
    int end = top + page + 1;
    if (end > static_cast<int>(m_visible.size())) end = static_cast<int>(m_visible.size());

    std::vector<IconLoadState::Job> jobs;
    auto generation = m_scanGeneration.load();
    for (int row = top; row < end; ++row)
    {
        size_t appIndex = m_visible[static_cast<size_t>(row)];
        if (appIndex >= m_iconState.size() || m_iconState[appIndex] != 0) continue;
        m_iconState[appIndex] = 1;
        int iconCx = GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(m_dpi > 0 ? m_dpi : 96));
        int iconCy = GetSystemMetricsForDpi(SM_CYSMICON, static_cast<UINT>(m_dpi > 0 ? m_dpi : 96));
        if (iconCx <= 0) iconCx = 16;
        if (iconCy <= 0) iconCy = 16;
        jobs.push_back({ generation, appIndex, iconCx, iconCy, m_apps[appIndex] });
    }
    if (jobs.empty()) return;

    auto state = m_iconLoad;
    HWND hwnd = m_hwnd;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->queue.insert(
            state->queue.end(),
            std::make_move_iterator(jobs.begin()),
            std::make_move_iterator(jobs.end()));
        if (!state->running && !state->stop)
        {
            state->running = true;
            start = true;
        }
    }
    if (!start) return;

    std::thread([state, hwnd]() {
        ::WinUninstaller::Core::WinrtLease lease;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            if (::WinUninstaller::Core::WorkerShutdownRequested())
            {
                std::lock_guard<std::mutex> lock(state->mu);
                state->running = false;
                state->queue.clear();
                break;
            }
            std::vector<IconLoadState::Job> batch;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                if (state->stop || state->queue.empty())
                {
                    state->running = false;
                    break;
                }
                batch.swap(state->queue);
            }

            auto* payload = new IconBatch();
            payload->items.reserve(batch.size());
            for (auto const& job : batch)
            {
                IconBatch::Item item;
                item.generation = job.generation;
                item.index = job.index;
                item.cx = job.cx;
                item.icon = ::WinUninstaller::Core::LoadAppSmallIcon(job.app, job.cx, job.cy);
                payload->items.push_back(item);
            }
            if (payload->items.empty() ||
                !PostMessageW(hwnd, MainWindow::WM_APP_ICONS_DONE, 0, reinterpret_cast<LPARAM>(payload)))
            {
                for (auto& item : payload->items)
                {
                    if (item.icon) DestroyIcon(item.icon);
                }
                delete payload;
            }
        }
        CoUninitialize();
    }).detach();
}

void MainWindow::ApplyLoadedIcons(IconBatch* batch)
{
    if (!batch) return;

    auto release = [&]() {
        for (auto& item : batch->items)
        {
            if (item.icon) DestroyIcon(item.icon);
        }
        delete batch;
    };

    if (!m_imageList)
    {
        release();
        return;
    }

    int expectedCx = GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(m_dpi > 0 ? m_dpi : 96));
    bool reloadIcons = false;
    for (auto& item : batch->items)
    {
        if (item.generation != m_scanGeneration || item.index >= m_apps.size() ||
            item.index >= m_appIconIndex.size() || (item.cx > 0 && item.cx != expectedCx))
        {
            if (item.icon) DestroyIcon(item.icon);
            item.icon = nullptr;
            if (item.index < m_iconState.size() && item.generation == m_scanGeneration && item.cx != expectedCx)
            {
                m_iconState[item.index] = 0;
                reloadIcons = true;
            }
            continue;
        }
        m_appIconIndex[item.index] = IconIndexForApp(item.index, item.icon);
        item.icon = nullptr;
        if (item.index < m_iconState.size()) m_iconState[item.index] = 2;

        if (!m_list) continue;
        for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
        {
            if (m_visible[static_cast<size_t>(row)] != item.index) continue;
            LVITEMW lv{};
            lv.mask = LVIF_IMAGE;
            lv.iItem = row;
            lv.iImage = m_appIconIndex[item.index];
            ListView_SetItem(m_list, &lv);
            break;
        }
    }
    delete batch;
    if (reloadIcons) RequestVisibleIcons();
}

int MainWindow::IconIndexForApp(size_t appIndex, HICON preloaded)
{
    if (appIndex >= m_apps.size() || !m_imageList) return m_defaultIconIndex;

    auto const& app = m_apps[appIndex];
    std::wstring key = app.DisplayIconPath + L"\n" + app.InstallLocation + L"\n" + app.DisplayName;
    auto it = m_iconCache.find(key);
    if (it != m_iconCache.end())
    {
        if (preloaded) DestroyIcon(preloaded);
        return it->second;
    }

    int index = m_defaultIconIndex;
    if (preloaded)
    {
        int added = ImageList_AddIcon(m_imageList, preloaded);
        if (added >= 0) index = added;
        DestroyIcon(preloaded);
    }
    m_iconCache.emplace(std::move(key), index);
    return index;
}

void MainWindow::UpdateHeaderSortArrows()
{
    HWND header = ListView_GetHeader(m_list);
    if (!header) return;

    int count = Header_GetItemCount(header);
    for (int i = 0; i < count; ++i)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, i, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == m_sortColumn)
        {
            item.fmt |= m_sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header, i, &item);
    }
}

void MainWindow::ApplyFilter(bool keepPlace)
{
    int keepApp = -1;
    size_t keepTopApp = static_cast<size_t>(-1);
    if (keepPlace && m_list)
    {
        keepApp = SelectedCoreIndex();
        int top = ListView_GetTopIndex(m_list);
        if (top >= 0 && static_cast<size_t>(top) < m_visible.size())
        {
            keepTopApp = m_visible[static_cast<size_t>(top)];
        }
    }

    std::wstring query = ToLowerCopy(m_searchQuery);

    m_visible.clear();
    m_visible.reserve(m_apps.size());
    for (size_t i = 0; i < m_apps.size(); ++i)
    {
        auto const& app = m_apps[i];
        bool isMsix = app.SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
        if (m_category == 0 && isMsix) continue;
        if (m_category == 1 && !isMsix) continue;

        if (!query.empty())
        {
            auto name = ToLowerCopy(app.DisplayName);
            if (name.find(query) == std::wstring::npos) continue;
        }
        m_visible.push_back(i);
    }

    SortVisible();
    UpdateHeaderSortArrows();

    SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(m_list);
    for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
    {
        size_t appIndex = m_visible[static_cast<size_t>(row)];
        auto const& app = m_apps[appIndex];
        std::wstring name = app.DisplayName;
        if (app.IsOrphaned) name += T(::WinUninstaller::Ui::StrId::OrphanSuffix);

        int iconIndex = m_defaultIconIndex;
        if (appIndex < m_appIconIndex.size()) iconIndex = m_appIconIndex[appIndex];

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = row;
        item.pszText = name.data();
        item.iImage = iconIndex;
        item.lParam = static_cast<LPARAM>(appIndex);
        ListView_InsertItem(m_list, &item);
        ListView_SetItemText(m_list, row, 1, const_cast<wchar_t*>(app.FormattedSize.c_str()));
        bool isMsix = app.SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
        wchar_t const* typeText = T(isMsix
            ? ::WinUninstaller::Ui::StrId::TypeStore
            : ::WinUninstaller::Ui::StrId::TypeDesktop);
        ListView_SetItemText(m_list, row, 2, const_cast<wchar_t*>(typeText));
        ListView_SetItemText(m_list, row, 3, const_cast<wchar_t*>(app.InstallDate.c_str()));
        ListView_SetItemText(m_list, row, 4, const_cast<wchar_t*>(app.Publisher.c_str()));
    }
    SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_list, nullptr, TRUE);
    UpdateSummary();
    UpdateMenuState();
    RequestVisibleIcons();

    if (!keepPlace || !m_list || m_visible.empty()) return;
    int topRow = -1;
    int selectRow = -1;
    for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
    {
        size_t appIndex = m_visible[static_cast<size_t>(row)];
        if (keepApp >= 0 && appIndex == static_cast<size_t>(keepApp)) selectRow = row;
        if (keepTopApp != static_cast<size_t>(-1) && appIndex == keepTopApp) topRow = row;
    }
    if (topRow >= 0)
    {
        ListView_EnsureVisible(m_list, static_cast<int>(m_visible.size()) - 1, FALSE);
        ListView_EnsureVisible(m_list, topRow, FALSE);
    }
    if (selectRow >= 0)
    {
        ListView_SetItemState(
            m_list, selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

LRESULT MainWindow::OnListCustomDraw(LPNMLVCUSTOMDRAW lvcd)
{
    switch (lvcd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
    {
        int row = static_cast<int>(lvcd->nmcd.dwItemSpec);
        bool selected = (ListView_GetItemState(m_list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        if (selected)
        {
            // Native LVS_EX_FULLROWSELECT already fills icon + left gutter.
            return m_searchQuery.empty()
                ? CDRF_DODEFAULT
                : CDRF_NOTIFYSUBITEMDRAW;
        }

        lvcd->clrText = GetSysColor(COLOR_WINDOWTEXT);
        lvcd->clrTextBk = ((row % 2) == 1) ? kListAlt : GetSysColor(COLOR_WINDOW);
        return m_searchQuery.empty()
            ? CDRF_NEWFONT
            : (CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT);
    }
    case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
    {
        // Only used for search-term highlighting on the name column.
        if (lvcd->iSubItem != 0 || m_searchQuery.empty())
        {
            return CDRF_DODEFAULT;
        }

        int row = static_cast<int>(lvcd->nmcd.dwItemSpec);
        bool selected = (ListView_GetItemState(m_list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        auto appIndex = static_cast<size_t>(lvcd->nmcd.lItemlParam);
        if (appIndex >= m_apps.size()) return CDRF_DODEFAULT;
        std::wstring text = m_apps[appIndex].DisplayName;
        if (m_apps[appIndex].IsOrphaned) text += T(::WinUninstaller::Ui::StrId::OrphanSuffix);

        RECT rcBounds{};
        ListView_GetSubItemRect(m_list, row, 0, LVIR_BOUNDS, &rcBounds);
        RECT rcLabel{};
        ListView_GetSubItemRect(m_list, row, 0, LVIR_LABEL, &rcLabel);
        RECT rcIcon{};
        ListView_GetSubItemRect(m_list, row, 0, LVIR_ICON, &rcIcon);

        COLORREF bk = selected
            ? GetSysColor(COLOR_HIGHLIGHT)
            : (((row % 2) == 1) ? kListAlt : GetSysColor(COLOR_WINDOW));

        // SKIPDEFAULT skips icon too — paint column background + icon ourselves.
        HBRUSH brush = CreateSolidBrush(bk);
        FillRect(lvcd->nmcd.hdc, &rcBounds, brush);
        DeleteObject(brush);

        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = row;
        if (ListView_GetItem(m_list, &item) && m_imageList && item.iImage >= 0)
        {
            int iconY = rcIcon.top +
                ((rcIcon.bottom - rcIcon.top) - GetSystemMetrics(SM_CYSMICON)) / 2;
            ImageList_Draw(
                m_imageList, item.iImage, lvcd->nmcd.hdc,
                rcIcon.left, iconY, ILD_TRANSPARENT);
        }

        SetBkColor(lvcd->nmcd.hdc, bk);
        DrawHighlightedName(lvcd->nmcd.hdc, rcLabel, text, selected);
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

void MainWindow::DrawHighlightedName(HDC hdc, RECT const& rc, std::wstring const& text, bool selected)
{
    std::wstring query = ToLowerCopy(m_searchQuery);
    std::wstring lower = ToLowerCopy(text);

    COLORREF normal = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
    COLORREF match = selected ? kMatchSel : kMatch;
    COLORREF bk = selected ? GetSysColor(COLOR_HIGHLIGHT) : GetBkColor(hdc);

    SetBkMode(hdc, OPAQUE);
    int x = rc.left + 2;
    int y = rc.top;
    SIZE sz{};
    GetTextExtentPoint32W(hdc, L"Ay", 2, &sz);
    y += ((rc.bottom - rc.top) - sz.cy) / 2;

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t found = query.empty() ? std::wstring::npos : lower.find(query, pos);
        size_t end = (found == std::wstring::npos) ? text.size() : found;

        if (end > pos)
        {
            std::wstring chunk = text.substr(pos, end - pos);
            SetTextColor(hdc, normal);
            SetBkColor(hdc, bk);
            ExtTextOutW(
                hdc, x, y, ETO_CLIPPED | ETO_OPAQUE, &rc,
                chunk.c_str(), static_cast<int>(chunk.size()), nullptr);
            GetTextExtentPoint32W(hdc, chunk.c_str(), static_cast<int>(chunk.size()), &sz);
            x += sz.cx;
            pos = end;
        }

        if (found == std::wstring::npos) break;

        std::wstring hit = text.substr(found, query.size());
        SetTextColor(hdc, match);
        SetBkColor(hdc, bk);
        ExtTextOutW(
            hdc, x, y, ETO_CLIPPED | ETO_OPAQUE, &rc,
            hit.c_str(), static_cast<int>(hit.size()), nullptr);
        GetTextExtentPoint32W(hdc, hit.c_str(), static_cast<int>(hit.size()), &sz);
        x += sz.cx;
        pos = found + query.size();
    }
}

void MainWindow::SetStatusText(std::wstring const& text)
{
    if (!m_status) return;
    SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void MainWindow::UpdateSummary()
{
    if (!m_status) return;

    std::int64_t total = 0;
    for (size_t idx : m_visible) total += m_apps[idx].EstimatedSizeBytes;
    auto sizeText = ::WinUninstaller::Core::Format::FormatBytes(total);
    wchar_t buf[160]{};
    swprintf_s(
        buf,
        T(::WinUninstaller::Ui::StrId::StatusSummary),
        static_cast<unsigned>(m_visible.size()),
        sizeText.c_str());
    SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(buf));
}

int MainWindow::SelectedCoreIndex() const
{
    int sel = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(m_list, &item)) return -1;
    return static_cast<int>(item.lParam);
}

::WinUninstaller::Core::AppInfo const* MainWindow::SelectedApp() const
{
    int index = SelectedCoreIndex();
    if (index < 0 || static_cast<size_t>(index) >= m_apps.size()) return nullptr;
    return &m_apps[static_cast<size_t>(index)];
}

void MainWindow::ShowItemContextMenu(int x, int y)
{
    POINT ptClient{ x, y };
    ScreenToClient(m_list, &ptClient);
    LVHITTESTINFO hit{};
    hit.pt = ptClient;
    int row = ListView_HitTest(m_list, &hit);
    if (row >= 0)
    {
        ListView_SetItemState(m_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(m_list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_list, row, FALSE);
        UpdateMenuState();
    }

    auto* app = SelectedApp();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    using ::WinUninstaller::Ui::StrId;
    bool hasApp = app != nullptr && !m_busy;
    bool canPath = hasApp && app && !app->InstallLocation.empty();
    bool canUninstall = hasApp && ::WinUninstaller::Core::Format::CanExecuteUninstall(*app);
    bool canCleanup = hasApp && app->IsOrphaned && !app->RegistryKeyPath.empty();
    if (canCleanup)
    {
        AppendMenuW(menu, MF_STRING, IdCtxUninstall, T(StrId::CtxCleanup));
    }
    else
    {
        AppendMenuW(menu, MF_STRING | (canUninstall ? 0 : MF_GRAYED), IdCtxUninstall, T(StrId::CtxUninstall));
    }
    if (canPath)
    {
        AppendMenuW(menu, MF_STRING, IdCtxOpenFolder, T(StrId::CtxOpenFolder));
    }
    bool isStore = app && app->SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
    bool canReg = hasApp && app && !isStore && !app->RegistryKeyPath.empty();
    if (!isStore)
    {
        AppendMenuW(menu, MF_STRING | (canReg ? 0 : MF_GRAYED), IdCtxOpenReg, T(StrId::CtxOpenReg));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (hasApp ? 0 : MF_GRAYED), IdCtxCopyName, T(StrId::CtxCopyName));
    if (canPath)
    {
        AppendMenuW(menu, MF_STRING, IdCtxCopyPath, T(StrId::CtxCopyPath));
    }
    if (!isStore)
    {
        AppendMenuW(
            menu,
            MF_STRING | (canReg ? 0 : MF_GRAYED),
            IdCtxCopyReg,
            T(StrId::CtxCopyReg));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m_busy ? MF_GRAYED : 0), IdCtxRefresh, T(StrId::CtxRefresh));

    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, x, y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}

void MainWindow::CopyTextToClipboard(std::wstring const& text)
{
    if (text.empty()) return;
    if (!OpenClipboard(m_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem)
    {
        void* ptr = GlobalLock(mem);
        if (ptr)
        {
            memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        else
        {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void MainWindow::OpenInstallFolder()
{
    auto* app = SelectedApp();
    if (!app) return;

    std::wstring folder;
    if (!::WinUninstaller::Core::Format::TryResolveExistingDirectory(app->InstallLocation, folder))
    {
        std::wstring exe;
        if (::WinUninstaller::Core::Format::TryGetUninstallExecutable(app->UninstallString, exe))
        {
            auto slash = exe.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
            {
                ::WinUninstaller::Core::Format::TryResolveExistingDirectory(exe.substr(0, slash), folder);
            }
        }
    }

    if (folder.empty()) return;

    ShellExecuteW(m_hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::OpenRegistryKey()
{
    auto* app = SelectedApp();
    if (!app || app->RegistryKeyPath.empty()) return;

    // Regedit only reads LastKey on startup. Ask before closing a running instance.
    HWND regWnd = FindWindowW(L"RegEdit_RegEdit", nullptr);
    if (regWnd)
    {
        using ::WinUninstaller::Ui::StrId;
        int answer = MessageBoxCentered(
            m_hwnd,
            T(StrId::RegeditClosePrompt),
            T(StrId::AppTitle),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer != IDYES) return;

        DWORD pid = 0;
        GetWindowThreadProcessId(regWnd, &pid);
        PostMessageW(regWnd, WM_CLOSE, 0, 0);
        if (pid)
        {
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (process)
            {
                WaitForSingleObject(process, 5000);
                CloseHandle(process);
            }
        }
        if (FindWindowW(L"RegEdit_RegEdit", nullptr))
        {
            MessageBoxCentered(
                m_hwnd,
                T(StrId::RegeditStillOpen),
                T(StrId::AppTitle),
                MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    std::wstring hivePath = app->RegistryKeyPath;
    if (hivePath.rfind(L"Computer\\", 0) == 0)
    {
        hivePath = hivePath.substr(9);
    }
    else if (hivePath.rfind(L"计算机\\", 0) == 0)
    {
        hivePath = hivePath.substr(4);
    }

    // LastKey root label follows UI language ("Computer" / "计算机").
    std::wstring prefix = L"Computer\\";
    HKEY regeditKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
            0, KEY_READ, &regeditKey) == ERROR_SUCCESS)
    {
        wchar_t existing[1024]{};
        DWORD type = 0;
        DWORD size = sizeof(existing);
        if (RegQueryValueExW(
                regeditKey, L"LastKey", nullptr, &type,
                reinterpret_cast<LPBYTE>(existing), &size) == ERROR_SUCCESS &&
            type == REG_SZ)
        {
            std::wstring old = existing;
            auto pos = old.find(L"HKEY_");
            if (pos != std::wstring::npos && pos > 0)
            {
                prefix = old.substr(0, pos);
            }
        }
        RegCloseKey(regeditKey);
    }
    else if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE)
    {
        prefix = L"计算机\\";
    }

    std::wstring lastKey = prefix + hivePath;

    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &regeditKey, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(
            regeditKey, L"LastKey", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(lastKey.c_str()),
            static_cast<DWORD>((lastKey.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(regeditKey);
    }

    ShellExecuteW(m_hwnd, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::ReportUninstallResult(int exitCode, bool cleanup)
{
    if (exitCode == 0) return;
    if (!cleanup && exitCode == ERROR_INSTALL_USEREXIT) return;

    using ::WinUninstaller::Ui::StrId;
    wchar_t buf[320]{};
    wchar_t const* text = nullptr;
    UINT icon = MB_ICONWARNING;
    if (!cleanup && exitCode == static_cast<int>(HRESULT_FROM_WIN32(ERROR_PACKAGES_IN_USE)))
    {
        text = T(StrId::UninstallInUse);
    }
    else if (!cleanup &&
        (exitCode == ERROR_SUCCESS_REBOOT_REQUIRED || exitCode == ERROR_SUCCESS_REBOOT_INITIATED))
    {
        text = T(StrId::UninstallRestart);
        icon = MB_ICONINFORMATION;
    }
    else
    {
        swprintf_s(
            buf,
            T(cleanup ? StrId::CleanupFailed : StrId::UninstallFailed),
            exitCode,
            static_cast<unsigned>(exitCode));
        text = buf;
    }
    MessageBoxCentered(m_hwnd, text, T(StrId::AppTitle), MB_OK | icon);
}

namespace
{
    constexpr int kCleanupList = 1001;
    constexpr int kCleanupIcon = 1002;
    constexpr int kCleanupPrompt = 1003;
    constexpr int kCleanupPath = 1004;

    struct CleanupDialogState
    {
        HFONT font{ nullptr };
        std::wstring title;
        std::wstring prompt;
        std::wstring path;
        std::wstring nameHeader;
        std::wstring valueHeader;
        std::wstring cleanLabel;
        std::wstring cancelLabel;
        std::wstring defaultValue;
        std::wstring subkeyLabel;
        std::wstring binaryFormat;
        std::vector<::WinUninstaller::Core::UninstallRegistryEntry> entries;
        std::vector<::WinUninstaller::Core::UninstallRegistryEntry> selected;
        bool entireKey{ false };
        bool ready{ false };
    };

    std::wstring CleanupEntryName(CleanupDialogState const& state, ::WinUninstaller::Core::UninstallRegistryEntry const& entry)
    {
        if (entry.Subkey || !entry.Name.empty()) return entry.Name;
        return state.defaultValue;
    }

    std::wstring CleanupEntryValue(CleanupDialogState const& state, ::WinUninstaller::Core::UninstallRegistryEntry const& entry)
    {
        if (entry.Subkey) return state.subkeyLabel;
        if (entry.Binary)
        {
            int needed = _scwprintf(state.binaryFormat.c_str(), entry.BinaryBytes);
            if (needed < 0) return L"";
            std::wstring text(static_cast<size_t>(needed) + 1, L'\0');
            swprintf_s(text.data(), text.size(), state.binaryFormat.c_str(), entry.BinaryBytes);
            text.resize(static_cast<size_t>(needed));
            return text;
        }
        return entry.Text;
    }

    void LayoutCleanupDialog(HWND dialog)
    {
        RECT rc{};
        GetClientRect(dialog, &rc);
        int const pad = 12;
        int const icon = 32;
        int const promptH = 64;
        int textX = pad + icon + 8;
        int textW = rc.right - textX - pad;
        if (textW < 80) textW = 80;
        MoveWindow(GetDlgItem(dialog, kCleanupIcon), pad, pad, icon, icon, TRUE);
        MoveWindow(GetDlgItem(dialog, kCleanupPrompt), textX, pad, textW, promptH, TRUE);
        int pathY = pad + promptH + 4;
        MoveWindow(GetDlgItem(dialog, kCleanupPath), pad, pathY, rc.right - pad * 2, 20, TRUE);

        int const btnW = 88;
        int const btnH = 28;
        int btnY = rc.bottom - pad - btnH;
        int cancelX = rc.right - pad - btnW;
        int cleanX = cancelX - 8 - btnW;
        MoveWindow(GetDlgItem(dialog, IDCANCEL), cancelX, btnY, btnW, btnH, TRUE);
        MoveWindow(GetDlgItem(dialog, IDOK), cleanX, btnY, btnW, btnH, TRUE);

        int listY = pathY + 20 + 8;
        int listH = btnY - 8 - listY;
        if (listH < 60) listH = 60;
        HWND list = GetDlgItem(dialog, kCleanupList);
        MoveWindow(list, pad, listY, rc.right - pad * 2, listH, TRUE);
        RECT listRc{};
        GetClientRect(list, &listRc);
        int nameW = 160;
        int dataW = listRc.right - nameW - GetSystemMetrics(SM_CXVSCROLL);
        if (dataW < 80) dataW = 80;
        ListView_SetColumnWidth(list, 0, nameW);
        ListView_SetColumnWidth(list, 1, dataW);
    }

    void UpdateCleanupButton(HWND dialog)
    {
        HWND list = GetDlgItem(dialog, kCleanupList);
        int checked = 0;
        int count = ListView_GetItemCount(list);
        for (int i = 0; i < count; ++i)
        {
            if (ListView_GetCheckState(list, i)) ++checked;
        }
        EnableWindow(GetDlgItem(dialog, IDOK), checked > 0);
    }

    INT_PTR CALLBACK CleanupDialogProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* state = reinterpret_cast<CleanupDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<CleanupDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            SetWindowTextW(dialog, state->title.c_str());
            auto make = [&](wchar_t const* klass, wchar_t const* text, DWORD style, int id) {
                HWND child = CreateWindowExW(
                    0, klass, text, WS_CHILD | WS_VISIBLE | style,
                    0, 0, 0, 0, dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    nullptr, nullptr);
                if (child && state->font) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
                return child;
            };
            make(L"STATIC", nullptr, SS_ICON, kCleanupIcon);
            SendMessageW(GetDlgItem(dialog, kCleanupIcon), STM_SETICON, reinterpret_cast<WPARAM>(LoadIconW(nullptr, IDI_WARNING)), 0);
            make(L"STATIC", state->prompt.c_str(), SS_LEFT | SS_NOPREFIX, kCleanupPrompt);
            make(L"STATIC", state->path.c_str(), SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, kCleanupPath);
            HWND list = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCleanupList)), nullptr, nullptr);
            if (state->font)
            {
                SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
                SendMessageW(ListView_GetHeader(list), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            }
            ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.fmt = LVCFMT_LEFT;
            col.cx = 160;
            col.pszText = state->nameHeader.data();
            ListView_InsertColumn(list, 0, &col);
            col.cx = 280;
            col.pszText = state->valueHeader.data();
            ListView_InsertColumn(list, 1, &col);
            for (size_t i = 0; i < state->entries.size(); ++i)
            {
                auto name = CleanupEntryName(*state, state->entries[i]);
                auto value = CleanupEntryValue(*state, state->entries[i]);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(i);
                item.pszText = name.data();
                ListView_InsertItem(list, &item);
                ListView_SetItemText(list, item.iItem, 1, value.data());
                ListView_SetCheckState(list, item.iItem, TRUE);
            }
            make(L"BUTTON", state->cleanLabel.c_str(), WS_TABSTOP | BS_PUSHBUTTON, IDOK);
            make(L"BUTTON", state->cancelLabel.c_str(), WS_TABSTOP | BS_DEFPUSHBUTTON, IDCANCEL);
            SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);

            RECT windowRc{ 0, 0, 560, 420 };
            DWORD style = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE));
            DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE));
            AdjustWindowRectEx(&windowRc, style, FALSE, exStyle);
            SetWindowPos(
                dialog, nullptr, 0, 0,
                windowRc.right - windowRc.left, windowRc.bottom - windowRc.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutCleanupDialog(dialog);
            state->ready = true;
            UpdateCleanupButton(dialog);
            SetFocus(GetDlgItem(dialog, IDCANCEL));
            return FALSE;
        }
        case WM_SIZE:
            LayoutCleanupDialog(dialog);
            return TRUE;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 460;
            info->ptMinTrackSize.y = 320;
            return TRUE;
        }
        case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (!state || !state->ready || hdr->idFrom != kCleanupList || hdr->code != LVN_ITEMCHANGED)
            {
                return FALSE;
            }
            auto* changed = reinterpret_cast<LPNMLISTVIEW>(lParam);
            UINT oldCheck = (changed->uOldState & LVIS_STATEIMAGEMASK);
            UINT newCheck = (changed->uNewState & LVIS_STATEIMAGEMASK);
            if (oldCheck != newCheck) UpdateCleanupButton(dialog);
            return TRUE;
        }
        case WM_COMMAND:
            if (!state) return FALSE;
            if (LOWORD(wParam) == IDOK)
            {
                HWND list = GetDlgItem(dialog, kCleanupList);
                for (int i = 0; i < ListView_GetItemCount(list); ++i)
                {
                    if (ListView_GetCheckState(list, i) && static_cast<size_t>(i) < state->entries.size())
                    {
                        state->selected.push_back(state->entries[static_cast<size_t>(i)]);
                    }
                }
                if (state->selected.empty()) return TRUE;
                state->entireKey = state->selected.size() == state->entries.size();
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        default:
            return FALSE;
        }
    }

    bool ShowCleanupDialog(HWND owner, HINSTANCE instance, CleanupDialogState& state)
    {
        alignas(4) WORD buffer[16]{};
        auto* dlg = reinterpret_cast<DLGTEMPLATE*>(buffer);
        dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_MODALFRAME | DS_SETFOREGROUND;
        dlg->dwExtendedStyle = 0;
        dlg->cdit = 0;
        dlg->x = 0;
        dlg->y = 0;
        dlg->cx = 280;
        dlg->cy = 180;
        WORD* cursor = reinterpret_cast<WORD*>(dlg + 1);
        *cursor++ = 0;
        *cursor++ = 0;
        *cursor++ = 0;

        MessageBoxCenter::Owner() = owner;
        MessageBoxCenter::Hook() = SetWindowsHookExW(
            WH_CBT, MessageBoxCenter::Proc, nullptr, GetCurrentThreadId());
        INT_PTR result = DialogBoxIndirectParamW(
            instance, dlg, owner, CleanupDialogProc, reinterpret_cast<LPARAM>(&state));
        if (MessageBoxCenter::Hook())
        {
            UnhookWindowsHookEx(MessageBoxCenter::Hook());
            MessageBoxCenter::Hook() = nullptr;
            MessageBoxCenter::Owner() = nullptr;
        }
        return result == IDOK;
    }
}

void MainWindow::OnCleanup()
{
    int index = SelectedCoreIndex();
    if (index < 0 || static_cast<size_t>(index) >= m_apps.size()) return;
    if (m_busy) return;

    auto app = m_apps[static_cast<size_t>(index)];
    if (!app.IsOrphaned || app.RegistryKeyPath.empty()) return;

    using ::WinUninstaller::Ui::StrId;
    int needed = _scwprintf(T(StrId::CleanupConfirm), app.DisplayName.c_str());
    if (needed < 0) return;
    std::wstring prompt(static_cast<size_t>(needed) + 1, L'\0');
    swprintf_s(prompt.data(), prompt.size(), T(StrId::CleanupConfirm), app.DisplayName.c_str());
    prompt.resize(static_cast<size_t>(needed));

    CleanupDialogState state;
    state.font = m_font;
    state.title = T(StrId::AppTitle);
    state.prompt = std::move(prompt);
    state.path = app.RegistryKeyPath;
    state.nameHeader = T(StrId::ColName);
    state.valueHeader = T(StrId::CleanupValueHeader);
    state.cleanLabel = T(StrId::CtxCleanup);
    state.cancelLabel = T(StrId::CleanupCancel);
    state.defaultValue = T(StrId::CleanupDefaultValue);
    state.subkeyLabel = T(StrId::CleanupSubkey);
    state.binaryFormat = T(StrId::CleanupBinary);
    state.entries = ::WinUninstaller::Core::ListOrphanUninstallEntries(app);
    std::sort(state.entries.begin(), state.entries.end(), [](auto const& a, auto const& b) {
        return _wcsicmp(a.Name.c_str(), b.Name.c_str()) < 0;
    });

    if (!ShowCleanupDialog(m_hwnd, m_instance, state)) return;

    bool entireKey = state.entireKey;
    auto selected = std::move(state.selected);
    m_busy = true;
    UpdateMenuState();

    auto generation = ++m_scanGeneration;
    HWND hwnd = m_hwnd;
    std::thread([hwnd, generation, app, selected, entireKey]() {
        auto result = ::WinUninstaller::Core::RemoveOrphanUninstallEntry(app, selected, entireKey);
        if (::WinUninstaller::Core::WorkerShutdownRequested()) return;
        ::WinUninstaller::Core::WinrtLease lease;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }
        catch (...)
        {
        }
        auto* payload = new ScanDonePayload();
        payload->reportUninstall = true;
        payload->cleanup = true;
        payload->exitCode = result.Success ? 0 : result.ExitCode;
        try
        {
            payload->apps = ::WinUninstaller::Core::EnumerateApplications();
        }
        catch (...)
        {
        }
        if (!PostMessageW(hwnd, WM_APP_SCAN_DONE, generation, reinterpret_cast<LPARAM>(payload)))
        {
            delete payload;
        }
    }).detach();
}

void MainWindow::OnUninstall()
{
    int index = SelectedCoreIndex();
    if (index < 0 || static_cast<size_t>(index) >= m_apps.size()) return;
    if (m_busy) return;

    auto app = m_apps[static_cast<size_t>(index)];
    if (!::WinUninstaller::Core::Format::CanExecuteUninstall(app)) return;

    using ::WinUninstaller::Ui::StrId;
    {
        int promptNeeded = _scwprintf(T(StrId::UninstallStoreConfirm), app.DisplayName.c_str());
        if (promptNeeded < 0) return;
        std::wstring prompt(static_cast<size_t>(promptNeeded) + 1, L'\0');
        swprintf_s(prompt.data(), prompt.size(), T(StrId::UninstallStoreConfirm), app.DisplayName.c_str());
        prompt.resize(static_cast<size_t>(promptNeeded));

        TASKDIALOG_BUTTON buttons[] = {
            { IDYES, T(StrId::CtxUninstall) },
            { IDCANCEL, T(StrId::CleanupCancel) },
        };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = m_hwnd;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.pszWindowTitle = T(StrId::AppTitle);
        config.pszMainIcon = TD_WARNING_ICON;
        config.pszMainInstruction = prompt.c_str();
        config.pButtons = buttons;
        config.cButtons = 2;
        config.nDefaultButton = IDCANCEL;

        MessageBoxCenter::Owner() = m_hwnd;
        MessageBoxCenter::Hook() = SetWindowsHookExW(
            WH_CBT, MessageBoxCenter::Proc, nullptr, GetCurrentThreadId());
        int answer = 0;
        TaskDialogIndirect(&config, &answer, nullptr, nullptr);
        if (MessageBoxCenter::Hook())
        {
            UnhookWindowsHookEx(MessageBoxCenter::Hook());
            MessageBoxCenter::Hook() = nullptr;
            MessageBoxCenter::Owner() = nullptr;
        }
        if (answer != IDYES) return;
    }

    m_busy = true;
    UpdateMenuState();

    auto generation = ++m_scanGeneration;
    m_pendingUninstallName = app.DisplayName;
    int needed = _scwprintf(T(StrId::Uninstalling), app.DisplayName.c_str());
    if (needed > 0)
    {
        std::wstring status(static_cast<size_t>(needed) + 1, L'\0');
        swprintf_s(status.data(), status.size(), T(StrId::Uninstalling), app.DisplayName.c_str());
        status.resize(static_cast<size_t>(needed));
        SetStatusText(status);
    }
    SetTimer(m_hwnd, kUninstallTimerId, 12000, nullptr);

    HWND hwnd = m_hwnd;
    std::wstring uninstalling = T(StrId::Uninstalling);
    std::wstring refreshing = T(StrId::StatusRefreshing);
    bool storePackage = app.SourceType == ::WinUninstaller::Core::AppSourceType::MsixPackage;
    std::thread([hwnd, generation, app, uninstalling, refreshing, storePackage]() mutable {
        auto postStatus = [hwnd](std::wstring text) {
            auto* copy = new std::wstring(std::move(text));
            if (!PostMessageW(hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy)))
            {
                delete copy;
            }
        };
        auto postRefresh = [&](::WinUninstaller::Core::UninstallResult const& result) {
            if (::WinUninstaller::Core::WorkerShutdownRequested()) return;
            postStatus(refreshing);
            auto* payload = new ScanDonePayload();
            payload->reportUninstall = true;
            payload->exitCode = result.ExitCode;
            try
            {
                payload->apps = ::WinUninstaller::Core::EnumerateApplications();
            }
            catch (...)
            {
            }
            if (!PostMessageW(hwnd, WM_APP_SCAN_DONE, generation, reinterpret_cast<LPARAM>(payload)))
            {
                delete payload;
            }
        };
        auto run = [&]() {
            return ::WinUninstaller::Core::ExecuteUninstall(
                app,
                [&](unsigned percent) {
                    int count = _scwprintf(uninstalling.c_str(), app.DisplayName.c_str());
                    if (count < 0) return;
                    std::wstring text(static_cast<size_t>(count) + 8, L'\0');
                    swprintf_s(text.data(), text.size(), uninstalling.c_str(), app.DisplayName.c_str());
                    text.resize(static_cast<size_t>(wcslen(text.c_str())));
                    if (percent > 0 && percent <= 100)
                    {
                        text += L" ";
                        text += std::to_wstring(percent);
                        text += L"%";
                    }
                    postStatus(std::move(text));
                });
        };

        try
        {
            if (storePackage)
            {
                ::WinUninstaller::Core::WinrtLease lease;
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                }
                catch (...)
                {
                }
                auto result = run();
                postRefresh(result);
                return;
            }

            HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ::WinUninstaller::Core::UninstallResult result{ false, -1, L"Failed to start uninstaller." };
            try
            {
                result = run();
            }
            catch (...)
            {
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
            if (::WinUninstaller::Core::WorkerShutdownRequested()) return;

            ::WinUninstaller::Core::WinrtLease lease;
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            }
            catch (...)
            {
            }
            postRefresh(result);
        }
        catch (...)
        {
            if (::WinUninstaller::Core::WorkerShutdownRequested()) return;
            ::WinUninstaller::Core::WinrtLease lease;
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            }
            catch (...)
            {
            }
            postRefresh({ false, -1, L"Failed to start uninstaller." });
        }
    }).detach();
}
