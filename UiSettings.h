#pragma once

#include <Windows.h>

namespace WinUninstaller::Ui
{
    enum class Language : int
    {
        ZhCN = 0,
        EnUS = 1,
    };

    enum class StrId
    {
        AppTitle,
        MenuView,
        MenuHelp,
        Refresh,
        CategoryDesktop,
        CategoryStore,
        CategoryAll,
        Language,
        LangZh,
        LangEn,
        About,
        HomePage,
        Feedback,
        CheckUpdates,
        SearchCue,
        Clear,
        ColName,
        ColSize,
        ColDate,
        ColPublisher,
        ColType,
        TypeDesktop,
        TypeStore,
        CtxUninstall,
        CtxCleanup,
        CtxOpenFolder,
        CtxOpenReg,
        CtxCopyName,
        CtxCopyPath,
        CtxCopyReg,
        CtxRefresh,
        StatusSummary,
        OrphanSuffix,
        CleanupConfirm,
        CleanupDefaultValue,
        CleanupSubkey,
        CleanupValueHeader,
        CleanupCancel,
        CleanupBinary,
        CleanupFailed,
        UninstallStoreConfirm,
        Uninstalling,
        UninstallWaiting,
        UninstallInUse,
        StatusRefreshing,
        UninstallFailed,
        UninstallRestart,
        AboutTitle,
        AboutBody,
        CloseWhileUninstalling,
        RegeditClosePrompt,
        RegeditStillOpen,
    };

    struct Settings
    {
        Language language{ Language::ZhCN };
    };

    Settings LoadSettings();
    void SaveSettings(Settings const& settings);

    wchar_t const* Tr(Language lang, StrId id);
}
