#include "pch.h"
#include "Icons.h"
#include "AppScanner.h"

#include <shellapi.h>
#include <wincodec.h>

#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace WinUninstaller::Core
{
    namespace
    {
        std::wstring TrimQuotes(std::wstring s)
        {
            while (!s.empty() && (s.front() == L'"' || s.front() == L'\'')) s.erase(s.begin());
            while (!s.empty() && (s.back() == L'"' || s.back() == L'\'')) s.pop_back();
            return s;
        }

        void ParseIconSpec(std::wstring const& raw, std::wstring& path, int& index)
        {
            path = TrimQuotes(raw);
            index = 0;
            auto comma = path.rfind(L',');
            if (comma == std::wstring::npos) return;

            auto indexPart = path.substr(comma + 1);
            while (!indexPart.empty() && iswspace(indexPart.front())) indexPart.erase(indexPart.begin());
            if (indexPart.empty()) return;
            if (!iswdigit(indexPart[0]) && indexPart[0] != L'-') return;

            index = _wtoi(indexPart.c_str());
            path = TrimQuotes(path.substr(0, comma));
        }

        bool HasImageExtension(std::wstring const& path)
        {
            auto dot = path.rfind(L'.');
            if (dot == std::wstring::npos) return false;
            std::wstring ext = path.substr(dot);
            for (auto& c : ext) c = towlower(c);
            return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
                ext == L".bmp" || ext == L".gif" || ext == L".tif" || ext == L".tiff" ||
                ext == L".ico" || ext == L".webp";
        }

        int ScoreAssetFileName(std::wstring const& fileName)
        {
            std::wstring lower = fileName;
            for (auto& c : lower) c = towlower(c);
            if (lower.find(L"splash") != std::wstring::npos) return -100;
            if (lower.find(L"jumplist") != std::wstring::npos) return -100;
            if (lower.find(L"illustration") != std::wstring::npos) return -100;
            if (lower.find(L"contrast-") != std::wstring::npos) return -50;

            int score = 0;
            if (lower.find(L"applist") != std::wstring::npos) score += 80;
            if (lower.find(L"square44") != std::wstring::npos) score += 70;
            if (lower.find(L"storelogo") != std::wstring::npos) score += 60;
            if (lower.find(L"smalltile") != std::wstring::npos) score += 40;
            if (lower.find(L"targetsize-48") != std::wstring::npos) score += 30;
            else if (lower.find(L"scale-100") != std::wstring::npos) score += 15;
            return score;
        }

        std::wstring ResolveScaledAsset(std::wstring const& logicalPath)
        {
            DWORD attrs = GetFileAttributesW(logicalPath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                return logicalPath;
            }

            auto slash = logicalPath.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return {};
            auto dir = logicalPath.substr(0, slash);
            auto fileName = logicalPath.substr(slash + 1);
            auto dot = fileName.rfind(L'.');
            if (dot == std::wstring::npos) return {};
            auto stem = fileName.substr(0, dot);
            auto ext = fileName.substr(dot);

            static wchar_t const* kSuffixes[] = {
                L".targetsize-48",
                L".targetsize-48_altform-unplated",
                L".targetsize-32",
                L".scale-100",
                L".scale-200",
                L".targetsize-256",
            };
            for (auto suffix : kSuffixes)
            {
                auto candidate = dir + L'\\' + stem + suffix + ext;
                attrs = GetFileAttributesW(candidate.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                {
                    return candidate;
                }
            }

            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((dir + L'\\' + stem + L"*" + ext).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return {};
            std::wstring best;
            int bestScore = -1000;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                int score = ScoreAssetFileName(fd.cFileName);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = dir + L'\\' + fd.cFileName;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            return bestScore >= 0 ? best : std::wstring{};
        }

        std::wstring FindLogoImage(std::wstring const& folder)
        {
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
                auto resolved = ResolveScaledAsset(folder + L'\\' + candidate);
                if (!resolved.empty()) return resolved;
            }

            std::wstring assets = folder + L"\\Assets";
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((assets + L"\\*.png").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return {};
            std::wstring best;
            int bestScore = 0;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                int score = ScoreAssetFileName(fd.cFileName);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = assets + L'\\' + fd.cFileName;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            return best;
        }

        bool ResolveFilePath(
            std::wstring const& displayIconPath,
            std::wstring& outPath,
            int& outIndex)
        {
            outPath.clear();
            outIndex = 0;

            std::wstring path;
            int index = 0;
            ParseIconSpec(displayIconPath, path, index);
            if (path.empty()) return false;

            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES)
            {
                if (attrs & FILE_ATTRIBUTE_DIRECTORY)
                {
                    auto logo = FindLogoImage(path);
                    if (logo.empty()) return false;
                    outPath = logo;
                    return true;
                }

                outPath = path;
                outIndex = index;
                return true;
            }

            auto scaled = ResolveScaledAsset(path);
            if (scaled.empty()) return false;
            outPath = scaled;
            return true;
        }

        HICON LoadIconFromImageFile(std::wstring const& path, int cx, int cy)
        {
            IWICImagingFactory* factory = nullptr;
            if (FAILED(CoCreateInstance(
                    CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory))))
            {
                return nullptr;
            }

            IWICBitmapDecoder* decoder = nullptr;
            HRESULT hr = factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder);
            if (FAILED(hr) || !decoder)
            {
                factory->Release();
                return nullptr;
            }

            IWICBitmapFrameDecode* frame = nullptr;
            hr = decoder->GetFrame(0, &frame);
            if (FAILED(hr) || !frame)
            {
                decoder->Release();
                factory->Release();
                return nullptr;
            }

            IWICBitmapScaler* scaler = nullptr;
            factory->CreateBitmapScaler(&scaler);
            if (!scaler)
            {
                frame->Release();
                decoder->Release();
                factory->Release();
                return nullptr;
            }

            hr = scaler->Initialize(frame, cx, cy, WICBitmapInterpolationModeFant);
            if (FAILED(hr))
            {
                scaler->Release();
                frame->Release();
                decoder->Release();
                factory->Release();
                return nullptr;
            }

            IWICFormatConverter* converter = nullptr;
            factory->CreateFormatConverter(&converter);
            if (!converter ||
                FAILED(converter->Initialize(
                    scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            {
                if (converter) converter->Release();
                scaler->Release();
                frame->Release();
                decoder->Release();
                factory->Release();
                return nullptr;
            }

            UINT stride = static_cast<UINT>(cx * 4);
            UINT bufferSize = stride * static_cast<UINT>(cy);
            std::vector<BYTE> pixels(bufferSize);
            hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());

            converter->Release();
            scaler->Release();
            frame->Release();
            decoder->Release();
            factory->Release();

            if (FAILED(hr)) return nullptr;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = cx;
            bmi.bmiHeader.biHeight = -cy;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!color || !bits)
            {
                if (color) DeleteObject(color);
                return nullptr;
            }
            memcpy(bits, pixels.data(), bufferSize);

            HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr);
            if (!mask)
            {
                DeleteObject(color);
                return nullptr;
            }

            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmMask = mask;
            ii.hbmColor = color;
            HICON icon = CreateIconIndirect(&ii);
            DeleteObject(mask);
            DeleteObject(color);
            return icon;
        }
    }

    HICON LoadAppSmallIcon(AppInfo const& app, int cx, int cy)
    {
        std::wstring spec = app.DisplayIconPath;
        if (app.SourceType == AppSourceType::MsixPackage)
        {
            auto logo = ResolveMsixLogoFile(app.InstallLocation);
            if (!logo.empty()) spec = std::move(logo);
        }

        std::wstring path;
        int index = 0;
        if (!ResolveFilePath(spec, path, index))
        {
            return nullptr;
        }

        if (cx <= 0) cx = 16;
        if (cy <= 0) cy = 16;

        if (HasImageExtension(path))
        {
            if (HICON fromImage = LoadIconFromImageFile(path, cx, cy))
            {
                return fromImage;
            }
            // .ico may still work via ExtractIconEx below.
        }

        HICON large = nullptr;
        HICON smallIcon = nullptr;
        HRESULT extracted = SHDefExtractIconW(
            path.c_str(), index, 0, &large, &smallIcon, MAKELONG(cx, cy));
        HICON use = SUCCEEDED(extracted) ? (smallIcon ? smallIcon : large) : nullptr;
        if (use)
        {
            HICON copy = CopyIcon(use);
            if (large) DestroyIcon(large);
            if (smallIcon && smallIcon != large) DestroyIcon(smallIcon);
            return copy;
        }
        if (large) DestroyIcon(large);
        if (smallIcon && smallIcon != large) DestroyIcon(smallIcon);

        SHFILEINFOW sfi{};
        UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags) && sfi.hIcon)
        {
            return sfi.hIcon;
        }

        return nullptr;
    }
}
