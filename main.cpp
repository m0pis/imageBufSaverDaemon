#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <wincodec.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"

#ifndef BI_ALPHABITFIELDS
#define BI_ALPHABITFIELDS 6
#endif

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;
namespace fs = std::filesystem;

namespace {
constexpr int kNotifyWidth = 356;
constexpr int kNotifyHeight = 102;
constexpr int kSettingsWidth = 424;
constexpr int kSettingsHeight = 362;
constexpr int kScreenPadding = 18;
constexpr UINT_PTR kHideTimerId = 40;
constexpr UINT_PTR kClipboardRetryTimerId = 41;
constexpr UINT_PTR kSavedHideTimerId = 42;
constexpr UINT kShowSettingsMessage = WM_APP + 20;
constexpr UINT kHideTimerMs = 3700;
constexpr UINT kClipboardRetryMs = 140;
constexpr int kHotkeySettings = 1;
constexpr wchar_t kWindowClass[] = L"CringeBufSaverDaemonWindow";
constexpr wchar_t kMutexName[] = L"Local\\CringeBufSaverDaemon";

enum class PanelMode {
    Notify,
    Settings,
};

struct Settings {
    std::wstring saveFolder;
    std::wstring accentColor = L"#B0B4FF";
    bool autoStart = false;
};

HWND g_window = nullptr;
UINT g_pngClipboardFormat = 0;
DWORD g_lastClipboardSequence = 0;
int g_clipboardRetryCount = 0;
PanelMode g_panelMode = PanelMode::Notify;
HMONITOR g_targetMonitor = nullptr;
Settings g_settings;

std::atomic_bool g_webReady{ false };
std::atomic_bool g_pendingNotify{ false };
bool g_windowVisible = false;
bool g_showSettingsOnReady = true;

wil::com_ptr<ICoreWebView2Controller> g_webviewController;
wil::com_ptr<ICoreWebView2> g_webview;

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\r': break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring GetLocalAppDataPath() {
    PWSTR rawPath = nullptr;
    std::wstring result = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)) && rawPath) {
        result = rawPath;
    }
    CoTaskMemFree(rawPath);
    return result;
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID id, const std::wstring& fallback) {
    PWSTR rawPath = nullptr;
    std::wstring result = fallback;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &rawPath)) && rawPath) {
        result = rawPath;
    }
    CoTaskMemFree(rawPath);
    return result;
}

std::wstring GetExeDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    return fs::path(std::wstring(buffer.data(), size)).parent_path().wstring();
}

fs::path GetConfigPath() {
    return fs::path(GetExeDirectory()) / L"cringebufsaver.ini";
}

fs::path GetStartupShortcutPath() {
    return fs::path(GetKnownFolder(FOLDERID_Startup, GetExeDirectory())) / L"CringeBufSaverDaemon.lnk";
}

std::wstring GetDefaultSaveFolder() {
    return (fs::path(GetKnownFolder(FOLDERID_Pictures, GetExeDirectory())) / L"CringeBufSaver").wstring();
}

bool IsHexColor(const std::wstring& value) {
    if (value.size() != 7 || value[0] != L'#') {
        return false;
    }

    for (size_t i = 1; i < value.size(); ++i) {
        wchar_t ch = value[i];
        const bool digit = ch >= L'0' && ch <= L'9';
        const bool upper = ch >= L'A' && ch <= L'F';
        const bool lower = ch >= L'a' && ch <= L'f';
        if (!digit && !upper && !lower) {
            return false;
        }
    }

    return true;
}

std::wstring GetWebViewUserDataPath() {
    fs::path path = fs::path(GetLocalAppDataPath()) / L"CringeBufSaverDaemon" / L"WebView2";
    std::error_code ignored;
    fs::create_directories(path, ignored);
    return path.wstring();
}

std::wstring GetWebCopyAssetsPath() {
    fs::path exeDir = GetExeDirectory();
    std::vector<fs::path> candidates = {
        exeDir / L"web-copy" / L"assets",
        exeDir.parent_path() / L"web-copy" / L"assets",
        exeDir.parent_path().parent_path() / L"web-copy" / L"assets",
        exeDir.parent_path().parent_path().parent_path() / L"web-copy" / L"assets",
        exeDir.parent_path().parent_path().parent_path().parent_path() / L"web-copy" / L"assets",
    };

    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate / L"inter_medium.ttf")) {
            return candidate.wstring();
        }
    }

    return L"";
}

void LoadSettings() {
    g_settings.saveFolder = GetDefaultSaveFolder();
    g_settings.accentColor = L"#B0B4FF";
    g_settings.autoStart = fs::exists(GetStartupShortcutPath());
    std::wifstream file(GetConfigPath());
    if (!file) {
        return;
    }

    std::wstring line;
    while (std::getline(file, line)) {
        constexpr wchar_t folderKey[] = L"SaveFolder=";
        constexpr wchar_t accentKey[] = L"AccentColor=";
        constexpr wchar_t autoStartKey[] = L"AutoStart=";
        if (line.rfind(folderKey, 0) == 0) {
            std::wstring value = line.substr(std::wcslen(folderKey));
            if (!value.empty()) {
                g_settings.saveFolder = value;
            }
        }
        else if (line.rfind(accentKey, 0) == 0) {
            std::wstring value = line.substr(std::wcslen(accentKey));
            if (IsHexColor(value)) {
                g_settings.accentColor = value;
            }
        }
        else if (line.rfind(autoStartKey, 0) == 0) {
            g_settings.autoStart = line.substr(std::wcslen(autoStartKey)) == L"1";
        }
    }

    g_settings.autoStart = fs::exists(GetStartupShortcutPath());
}

void SaveSettings() {
    std::wofstream file(GetConfigPath(), std::ios::trunc);
    if (file) {
        file << L"SaveFolder=" << g_settings.saveFolder << L"\n";
        file << L"AccentColor=" << g_settings.accentColor << L"\n";
        file << L"AutoStart=" << (g_settings.autoStart ? L"1" : L"0") << L"\n";
    }
}

bool CreateStartupShortcut() {
    wil::com_ptr<IShellLinkW> shellLink;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
    if (FAILED(hr)) {
        return false;
    }

    fs::path exePath = fs::path(GetExeDirectory()) / L"CringeBufSaverDaemon.exe";
    shellLink->SetPath(exePath.wstring().c_str());
    shellLink->SetWorkingDirectory(GetExeDirectory().c_str());
    shellLink->SetDescription(L"CringeBufSaverDaemon");

    wil::com_ptr<IPersistFile> persistFile;
    hr = shellLink->QueryInterface(IID_PPV_ARGS(&persistFile));
    if (FAILED(hr)) {
        return false;
    }

    fs::path shortcutPath = GetStartupShortcutPath();
    std::error_code ignored;
    fs::create_directories(shortcutPath.parent_path(), ignored);
    return SUCCEEDED(persistFile->Save(shortcutPath.wstring().c_str(), TRUE));
}

bool RemoveStartupShortcut() {
    std::error_code ignored;
    fs::remove(GetStartupShortcutPath(), ignored);
    return !fs::exists(GetStartupShortcutPath());
}

bool SetAutoStartEnabled(bool enabled) {
    bool ok = enabled ? CreateStartupShortcut() : RemoveStartupShortcut();
    if (ok) {
        g_settings.autoStart = enabled;
        SaveSettings();
    }
    return ok;
}

void PostJson(const std::wstring& json) {
    if (g_webReady && g_webview) {
        g_webview->PostWebMessageAsJson(json.c_str());
    }
}

HMONITOR MonitorFromCurrentCursor() {
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_window) {
        return MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    }

    return MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST);
}

RECT GetMonitorWorkArea(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    return workArea;
}

void ResizeWebView() {
    if (!g_window || !g_webviewController) {
        return;
    }

    RECT bounds{};
    GetClientRect(g_window, &bounds);
    g_webviewController->put_Bounds(bounds);
}

void MovePanelToBottomRight(int width, int height) {
    if (!g_targetMonitor) {
        g_targetMonitor = MonitorFromCurrentCursor();
    }

    RECT workArea = GetMonitorWorkArea(g_targetMonitor);
    const int x = workArea.right - width - kScreenPadding;
    const int y = workArea.bottom - height - kScreenPadding;
    SetWindowPos(
        g_window,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 6, 6);
    if (region) {
        SetWindowRgn(g_window, region, TRUE);
    }
    ResizeWebView();
}

void HidePanel() {
    KillTimer(g_window, kHideTimerId);
    KillTimer(g_window, kSavedHideTimerId);
    g_windowVisible = false;
    ShowWindow(g_window, SW_HIDE);
}

void ShowPanel(PanelMode mode) {
    if (!g_window) {
        return;
    }

    g_panelMode = mode;
    const int width = mode == PanelMode::Notify ? kNotifyWidth : kSettingsWidth;
    const int height = mode == PanelMode::Notify ? kNotifyHeight : kSettingsHeight;
    MovePanelToBottomRight(width, height);
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    SetWindowPos(
        g_window,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_windowVisible = true;
}

void ShowNotify() {
    if (!g_webReady || !g_webview) {
        g_pendingNotify = true;
        return;
    }

    g_pendingNotify = false;
    ShowPanel(PanelMode::Notify);
    PostJson(L"{\"type\":\"clipboard_image\"}");
    KillTimer(g_window, kHideTimerId);
    KillTimer(g_window, kSavedHideTimerId);
    SetTimer(g_window, kHideTimerId, kHideTimerMs, nullptr);
}

void ShowSettingsPanel() {
    if (!g_webReady || !g_webview) {
        return;
    }

    KillTimer(g_window, kHideTimerId);
    KillTimer(g_window, kSavedHideTimerId);
    g_targetMonitor = MonitorFromCurrentCursor();
    ShowPanel(PanelMode::Settings);

    std::wstring json = L"{\"type\":\"settings\",\"folder\":\"" + JsonEscape(g_settings.saveFolder) +
        L"\",\"accent\":\"" + JsonEscape(g_settings.accentColor) +
        L"\",\"autoStart\":" + std::wstring(g_settings.autoStart ? L"true" : L"false") + L"}";
    PostJson(json);
}

void SendSettingsState(const std::wstring& message) {
    std::wstring json = L"{\"type\":\"settings_state\",\"folder\":\"" + JsonEscape(g_settings.saveFolder) +
        L"\",\"accent\":\"" + JsonEscape(g_settings.accentColor) +
        L"\",\"autoStart\":" + std::wstring(g_settings.autoStart ? L"true" : L"false") +
        L",\"message\":\"" + JsonEscape(message) + L"\"}";
    PostJson(json);
}

bool ClipboardHasImage() {
    return IsClipboardFormatAvailable(CF_BITMAP) ||
        IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_DIBV5) ||
        (g_pngClipboardFormat != 0 && IsClipboardFormatAvailable(g_pngClipboardFormat));
}

void CheckClipboardForImage() {
    if (ClipboardHasImage()) {
        KillTimer(g_window, kClipboardRetryTimerId);
        g_clipboardRetryCount = 0;
        ShowNotify();
        return;
    }

    if (g_clipboardRetryCount > 0) {
        --g_clipboardRetryCount;
        SetTimer(g_window, kClipboardRetryTimerId, kClipboardRetryMs, nullptr);
    }
}

void HandleClipboardUpdate() {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence != 0 && sequence == g_lastClipboardSequence) {
        return;
    }

    if (sequence != 0) {
        g_lastClipboardSequence = sequence;
    }

    g_targetMonitor = MonitorFromCurrentCursor();
    g_clipboardRetryCount = 10;
    CheckClipboardForImage();
}

fs::path BuildScreenshotPath() {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    std::wstringstream name;
    name << L"screenshot_"
        << time.wYear
        << (time.wMonth < 10 ? L"0" : L"") << time.wMonth
        << (time.wDay < 10 ? L"0" : L"") << time.wDay
        << L"_"
        << (time.wHour < 10 ? L"0" : L"") << time.wHour
        << (time.wMinute < 10 ? L"0" : L"") << time.wMinute
        << (time.wSecond < 10 ? L"0" : L"") << time.wSecond
        << L".png";

    fs::path folder = g_settings.saveFolder.empty() ? GetDefaultSaveFolder() : g_settings.saveFolder;
    fs::path target = folder / name.str();
    int suffix = 2;
    while (fs::exists(target)) {
        std::wstringstream retry;
        retry << name.str().substr(0, name.str().size() - 4) << L"_" << suffix++ << L".png";
        target = folder / retry.str();
    }
    return target;
}

bool WriteClipboardPngBytes(const fs::path& target) {
    if (g_pngClipboardFormat == 0 || !IsClipboardFormatAvailable(g_pngClipboardFormat)) {
        return false;
    }

    HANDLE data = GetClipboardData(g_pngClipboardFormat);
    if (!data) {
        return false;
    }

    SIZE_T size = GlobalSize(data);
    void* raw = GlobalLock(data);
    if (!raw || size == 0) {
        if (raw) {
            GlobalUnlock(data);
        }
        return false;
    }

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        GlobalUnlock(data);
        return false;
    }

    out.write(static_cast<const char*>(raw), static_cast<std::streamsize>(size));
    GlobalUnlock(data);
    return out.good();
}

int DibColorTableEntries(const BITMAPINFOHEADER* header) {
    if (header->biClrUsed != 0) {
        return static_cast<int>(header->biClrUsed);
    }

    if (header->biBitCount <= 8) {
        return 1 << header->biBitCount;
    }

    return 0;
}

void* GetDibBitsPointer(void* dib) {
    auto* header = static_cast<BITMAPINFOHEADER*>(dib);
    BYTE* cursor = static_cast<BYTE*>(dib) + header->biSize;

    if (header->biCompression == BI_BITFIELDS && header->biSize == sizeof(BITMAPINFOHEADER)) {
        cursor += sizeof(DWORD) * 3;
    }
    else if (header->biCompression == BI_ALPHABITFIELDS && header->biSize == sizeof(BITMAPINFOHEADER)) {
        cursor += sizeof(DWORD) * 4;
    }

    cursor += DibColorTableEntries(header) * sizeof(RGBQUAD);
    return cursor;
}

HBITMAP CreateBitmapFromClipboardDib(UINT format) {
    HANDLE data = GetClipboardData(format);
    if (!data) {
        return nullptr;
    }

    void* raw = GlobalLock(data);
    if (!raw) {
        return nullptr;
    }

    auto* info = static_cast<BITMAPINFO*>(raw);
    void* bits = GetDibBitsPointer(raw);
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBitmap(screen, &info->bmiHeader, CBM_INIT, bits, info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    GlobalUnlock(data);
    return bitmap;
}

bool EncodeBitmapToPng(HBITMAP bitmap, const fs::path& target) {
    if (!bitmap) {
        return false;
    }

    wil::com_ptr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    wil::com_ptr<IWICBitmap> source;
    hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source);
    if (FAILED(hr)) {
        return false;
    }

    wil::com_ptr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        return false;
    }

    hr = stream->InitializeFromFilename(target.wstring().c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return false;
    }

    wil::com_ptr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        return false;
    }

    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return false;
    }

    wil::com_ptr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    source->GetSize(&width, &height);
    frame->SetSize(width, height);

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pixelFormat);

    hr = frame->WriteSource(source.get(), nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        return false;
    }

    return SUCCEEDED(encoder->Commit());
}

bool SaveClipboardImage(std::wstring& savedPath) {
    fs::path target = BuildScreenshotPath();
    std::error_code ignored;
    fs::create_directories(target.parent_path(), ignored);

    if (!OpenClipboard(g_window)) {
        return false;
    }

    bool saved = WriteClipboardPngBytes(target);
    if (!saved) {
        HBITMAP bitmap = nullptr;
        if (IsClipboardFormatAvailable(CF_BITMAP)) {
            bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
            saved = EncodeBitmapToPng(bitmap, target);
        }
        else if (IsClipboardFormatAvailable(CF_DIBV5)) {
            bitmap = CreateBitmapFromClipboardDib(CF_DIBV5);
            saved = EncodeBitmapToPng(bitmap, target);
            if (bitmap) {
                DeleteObject(bitmap);
            }
        }
        else if (IsClipboardFormatAvailable(CF_DIB)) {
            bitmap = CreateBitmapFromClipboardDib(CF_DIB);
            saved = EncodeBitmapToPng(bitmap, target);
            if (bitmap) {
                DeleteObject(bitmap);
            }
        }
    }

    CloseClipboard();

    if (saved) {
        savedPath = target.wstring();
    }
    return saved;
}

bool CopySavedFileToClipboard(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    if (!OpenClipboard(g_window)) {
        return false;
    }

    EmptyClipboard();

    const SIZE_T textBytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL textMemory = GlobalAlloc(GMEM_MOVEABLE, textBytes);
    if (textMemory) {
        void* textData = GlobalLock(textMemory);
        if (textData) {
            memcpy(textData, path.c_str(), textBytes);
            GlobalUnlock(textMemory);
            if (!SetClipboardData(CF_UNICODETEXT, textMemory)) {
                GlobalFree(textMemory);
            }
        }
        else {
            GlobalFree(textMemory);
        }
    }

    const SIZE_T pathListBytes = (path.size() + 2) * sizeof(wchar_t);
    const SIZE_T dropBytes = sizeof(DROPFILES) + pathListBytes;
    HGLOBAL dropMemory = GlobalAlloc(GMEM_MOVEABLE, dropBytes);
    if (dropMemory) {
        void* dropData = GlobalLock(dropMemory);
        if (dropData) {
            ZeroMemory(dropData, dropBytes);
            auto* dropFiles = static_cast<DROPFILES*>(dropData);
            dropFiles->pFiles = sizeof(DROPFILES);
            dropFiles->fWide = TRUE;
            wchar_t* fileList = reinterpret_cast<wchar_t*>(static_cast<BYTE*>(dropData) + sizeof(DROPFILES));
            memcpy(fileList, path.c_str(), path.size() * sizeof(wchar_t));
            GlobalUnlock(dropMemory);
            if (!SetClipboardData(CF_HDROP, dropMemory)) {
                GlobalFree(dropMemory);
            }
        }
        else {
            GlobalFree(dropMemory);
        }
    }

    CloseClipboard();
    return true;
}

void SendSaveResult(bool ok, const std::wstring& path) {
    std::wstring json = L"{\"type\":\"save_result\",\"ok\":" + std::wstring(ok ? L"true" : L"false") +
        L",\"path\":\"" + JsonEscape(path) + L"\"}";
    PostJson(json);

    if (ok) {
        KillTimer(g_window, kHideTimerId);
        SetTimer(g_window, kSavedHideTimerId, 1400, nullptr);
    }
}

bool PickFolder(std::wstring& folder) {
    wil::com_ptr<IFileDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
        return false;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose screenshot save folder");

    if (!g_settings.saveFolder.empty() && fs::exists(g_settings.saveFolder)) {
        wil::com_ptr<IShellItem> currentFolder;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_settings.saveFolder.c_str(), nullptr, IID_PPV_ARGS(&currentFolder)))) {
            dialog->SetFolder(currentFolder.get());
        }
    }

    hr = dialog->Show(g_window);
    if (FAILED(hr)) {
        return false;
    }

    wil::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return false;
    }

    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
        return false;
    }

    folder = rawPath;
    CoTaskMemFree(rawPath);
    return true;
}

std::wstring BuildHtml() {
    return std::wstring(LR"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
@font-face {
  font-family: "Toronto Inter";
  src: url("https://cringebufsaver.assets/inter_medium.ttf") format("truetype");
  font-weight: 500;
}
@font-face {
  font-family: "Toronto Inter";
  src: url("https://cringebufsaver.assets/inter_semibold.ttf") format("truetype");
  font-weight: 600;
}
:root {
  --layout: rgb(25, 25, 28);
  --white: rgb(255, 255, 255);
  --black: rgb(0, 0, 0);
  --accent: rgb(176, 180, 255);
  --child: rgb(28, 28, 33);
  --widget: rgb(33, 33, 40);
  --text: rgb(110, 110, 129);
  --border: rgb(35, 35, 44);
  --ok: rgb(139, 220, 154);
  --bad: rgb(255, 117, 117);
  --ease: cubic-bezier(.18, .9, .22, 1);
}
* { box-sizing: border-box; }
html, body { width: 100%; height: 100%; }
body {
  margin: 0;
  overflow: hidden;
  background: transparent;
  color: var(--white);
  font-family: "Toronto Inter", "Segoe UI", Arial, sans-serif;
  font-size: 12px;
  letter-spacing: 0;
  user-select: none;
}
button, input { font: inherit; }
button { border: 0; color: inherit; cursor: pointer; }
.stage {
  width: 100%;
  height: 100%;
  display: grid;
  place-items: center;
  padding: 0;
}
.panel {
  position: absolute;
  opacity: 0;
  transform: none;
  pointer-events: none;
  transition: opacity 260ms ease, transform 360ms var(--ease);
}
.panel.is-visible {
  opacity: 1;
  transform: none;
  pointer-events: auto;
}
.panel.is-closing {
  opacity: 1;
  transform: none;
  pointer-events: none;
  filter: brightness(.92);
}
.panel.is-closing .content,
.panel.is-closing .indicator,
.panel.is-closing .settings-head,
.panel.is-closing .settings-card {
  opacity: .45;
  transform: translateY(4px);
  transition: opacity 180ms ease, transform 180ms var(--ease);
}
.notify {
  width: 100%;
  height: 100%;
  overflow: hidden;
  display: grid;
  grid-template-columns: 74px 1fr;
  align-items: center;
  gap: 10px;
  padding: 10px;
  border: 1px solid rgba(255, 255, 255, .045);
  border-radius: 3px;
  background: rgba(25, 25, 28, .96);
  box-shadow: 0 18px 44px rgba(0, 0, 0, .48);
}
.notify:active { transform: scale(.99); }
.indicator {
  width: 74px;
  height: 74px;
  display: grid;
  place-items: center;
  border-radius: 2px;
  background: var(--child);
  border: 1px solid var(--border);
}
#cube { width: 58px; height: 58px; }
.content {
  min-width: 0;
  display: flex;
  flex-direction: column;
  justify-content: center;
  padding-right: 4px;
  transition: opacity 180ms ease, transform 180ms var(--ease);
}
.topline {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}
.title {
  margin: 0;
  color: var(--white);
  font-size: 13px;
  line-height: 16px;
  font-weight: 600;
}
.tag {
  flex: 0 0 auto;
  height: 20px;
  display: grid;
  place-items: center;
  padding: 0 8px;
  border-radius: 2px;
  background: var(--widget);
  color: var(--accent);
  font-size: 10px;
  font-weight: 600;
}
.description {
  margin: 4px 0 0;
  color: var(--text);
  font-size: 11px;
  line-height: 15px;
  font-weight: 500;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.progress {
  height: 12px;
  margin-top: 12px;
  position: relative;
  overflow: hidden;
  border-radius: 999px;
  background: var(--widget);
}
.progress span {
  display: block;
  width: 100%;
  height: 100%;
  border-radius: inherit;
  background: var(--accent);
  transform-origin: left center;
}
.notify.is-visible .progress span { animation: drain 3000ms linear forwards; }
@keyframes drain {
  from { transform: scaleX(1); }
  to { transform: scaleX(0); }
}
.settings {
  width: 100%;
  height: 100%;
  padding: 12px;
  border: 1px solid rgba(255, 255, 255, .045);
  border-radius: 3px;
  background: rgba(25, 25, 28, .97);
  box-shadow: 0 18px 44px rgba(0, 0, 0, .48);
}
.settings-head {
  height: 32px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 0 8px;
  transition: opacity 180ms ease, transform 180ms var(--ease);
}
.settings-title {
  margin: 0;
  color: var(--white);
  font-size: 13px;
  font-weight: 600;
}
.head-actions { display: flex; gap: 8px; }
.icon-btn {
  width: 28px;
  height: 28px;
  display: grid;
  place-items: center;
  border-radius: 2px;
  background: var(--child);
  color: var(--text);
  transition: background 180ms ease, color 180ms ease;
}
.icon-btn svg {
  width: 14px;
  height: 14px;
  stroke: currentColor;
  stroke-width: 2.4;
  fill: none;
  stroke-linecap: round;
  stroke-linejoin: round;
}
.icon-btn:hover { background: var(--widget); color: var(--white); }
.settings-card {
  height: calc(100% - 32px);
  border-radius: 2px;
  background: var(--child);
  padding: 10px;
  transition: opacity 180ms ease, transform 180ms var(--ease);
}
.settings-section {
  padding-bottom: 10px;
  margin-bottom: 10px;
  border-bottom: 1px solid var(--border);
}
.settings-section:last-child {
  padding-bottom: 0;
  margin-bottom: 0;
  border-bottom: 0;
}
.label {
  margin: 0 0 8px;
  color: var(--text);
  font-size: 11px;
  font-weight: 600;
}
.path-row {
  display: grid;
  grid-template-columns: 1fr 86px;
  gap: 10px;
}
.path-input {
  min-width: 0;
  height: 32px;
  padding: 0 10px;
  border: 0;
  outline: 0;
  border-radius: 2px;
  background: var(--widget);
  color: var(--white);
  font-size: 11px;
}
.action-btn {
  height: 32px;
  border-radius: 2px;
  background: var(--accent);
  color: var(--black);
  font-size: 11px;
  font-weight: 600;
}
.secondary {
  margin-top: 10px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
}
.secondary .action-btn {
  background: var(--widget);
  color: var(--white);
}
)HTML") + LR"HTML(
.toggle-row {
  min-height: 32px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  cursor: pointer;
}
.toggle-copy {
  min-width: 0;
}
.toggle-copy h3 {
  margin: 0;
  color: var(--white);
  font-size: 12px;
  line-height: 15px;
  font-weight: 600;
}
.toggle-copy p {
  margin: 1px 0 0;
  color: var(--text);
  font-size: 11px;
  line-height: 14px;
}
.switch {
  width: 30px;
  height: 16px;
  flex: 0 0 30px;
  padding: 0;
  border-radius: 999px;
  background: var(--widget);
  transition: background 240ms var(--ease);
}
.switch span {
  display: block;
  width: 12px;
  height: 12px;
  margin: 2px;
  border-radius: 50%;
  background: var(--text);
  transform: translateX(0);
  transition: transform 240ms var(--ease), background 240ms var(--ease);
}
.switch.is-on {
  background: var(--accent);
}
.switch.is-on span {
  background: var(--black);
  transform: translateX(14px);
}
.mini-sliders {
  display: grid;
  grid-template-columns: 1fr 1fr 2fr;
  gap: 10px;
  margin-top: 13px;
}
.mini {
  --pos: 68%;
  height: 12px;
  border-radius: 999px;
  position: relative;
  cursor: pointer;
}
.mini span {
  display: block;
  width: 100%;
  height: 100%;
  border-radius: inherit;
}
.mini i {
  width: 6px;
  height: 6px;
  position: absolute;
  top: 3px;
  left: clamp(3px, calc(var(--pos) - 3px), calc(100% - 9px));
  border: 2px solid var(--white);
  border-radius: 50%;
  box-shadow: 0 0 6px var(--black);
}
.saturation span {
  background: linear-gradient(90deg, #fff, var(--accent));
}
.brightness span {
  background: linear-gradient(90deg, #000, #fff);
}
.hue span {
  background: linear-gradient(90deg, #f55, #ff5, #5f5, #5ff, #55f, #f5f, #f55);
}
.hint {
  margin: 10px 0 0;
  color: var(--text);
  font-size: 11px;
  line-height: 14px;
}
.status {
  margin-top: 10px;
  min-height: 18px;
  color: var(--text);
  font-size: 11px;
  line-height: 18px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.status.ok { color: var(--ok); }
.status.bad { color: var(--bad); }
</style>
</head>
<body>
  <main class="stage">
    <section class="panel notify" id="notify" aria-live="polite">
      <div class="indicator"><canvas id="cube" width="116" height="116"></canvas></div>
      <div class="content">
        <div class="topline">
          <h1 class="title" id="notifyTitle">Image in clipboard</h1>
          <div class="tag" id="notifyTag">READY</div>
        </div>
        <p class="description" id="notifyDesc">Click to save it as PNG.</p>
        <div class="progress"><span id="progressFill"></span></div>
      </div>
    </section>

)HTML" + LR"HTML(
    <section class="panel settings" id="settings">
      <div class="settings-head">
        <h2 class="settings-title">CringeBufSaver</h2>
        <div class="head-actions">
          <button class="icon-btn" id="closeSettings" title="Close" aria-label="Close settings">
            <svg viewBox="0 0 24 24"><path d="M18 6 6 18"></path><path d="m6 6 12 12"></path></svg>
          </button>
          <button class="icon-btn" id="exitApp" title="Exit" aria-label="Exit daemon">
            <svg viewBox="0 0 24 24"><path d="M12 2v10"></path><path d="M18.4 6.6a9 9 0 1 1-12.8 0"></path></svg>
          </button>
        </div>
      </div>
      <div class="settings-card">
        <div class="settings-section">
          <p class="label">Save folder</p>
          <div class="path-row">
            <input class="path-input" id="folderInput" spellcheck="false">
            <button class="action-btn" id="browseFolder">Browse</button>
          </div>
          <div class="secondary">
            <button class="action-btn" id="saveFolder">Save path</button>
            <button class="action-btn" id="openFolder">Open folder</button>
          </div>
        </div>
        <div class="settings-section" id="autoStartControl">
          <div class="toggle-row">
            <div class="toggle-copy">
              <h3>Autostart</h3>
              <p>Run daemon when Windows starts</p>
            </div>
            <button class="switch" id="autoStartSwitch" aria-label="Toggle autostart"><span></span></button>
          </div>
        </div>
        <div class="settings-section">
          <p class="label">Accent color</p>
          <div class="mini-sliders" id="accentPicker">
            <div class="mini saturation"><span></span><i></i></div>
            <div class="mini brightness"><span></span><i></i></div>
            <div class="mini hue"><span></span><i></i></div>
          </div>
        </div>
        <p class="hint">Ctrl + Shift + Space opens this panel. Click the clipboard notification to save the current image.</p>
        <div class="status" id="settingsStatus"></div>
      </div>
    </section>
  </main>

)HTML" + LR"HTML(
<script>
const notify = document.getElementById("notify");
const settings = document.getElementById("settings");
const progress = document.getElementById("progressFill");
const title = document.getElementById("notifyTitle");
const desc = document.getElementById("notifyDesc");
const tag = document.getElementById("notifyTag");
const folderInput = document.getElementById("folderInput");
const settingsStatus = document.getElementById("settingsStatus");
const autoStartControl = document.getElementById("autoStartControl");
const autoStartSwitch = document.getElementById("autoStartSwitch");
const accentPicker = document.getElementById("accentPicker");
const saturation = accentPicker.querySelector(".saturation");
const brightness = accentPicker.querySelector(".brightness");
const hue = accentPicker.querySelector(".hue");
const canvas = document.getElementById("cube");
const ctx = canvas.getContext("2d");

let hideTimer = 0;
let closeTimer = 0;
let rotationX = 0;
let rotationY = 0;
let speedMultiplier = 0;
let speedRising = true;
let lastFrame = performance.now();
const accentState = { h: 0.655, s: 0.31, v: 1 };

function rotateX(v, angle) {
  const rad = angle * (Math.PI / 180);
  const cosA = Math.cos(rad);
  const sinA = Math.sin(rad);
  return { x: v.x, y: v.y * cosA - v.z * sinA, z: v.y * sinA + v.z * cosA };
}
function rotateY(v, angle) {
  const rad = angle * (Math.PI / 180);
  const cosA = Math.cos(rad);
  const sinA = Math.sin(rad);
  return { x: v.x * cosA + v.z * sinA, y: v.y, z: -v.x * sinA + v.z * cosA };
}
function lerp(a, b, t) { return a + (b - a) * Math.min(1, Math.max(0, t)); }
function drawCube(now) {
  const dt = Math.min(0.05, (now - lastFrame) / 1000);
  lastFrame = now;
  const targetSpeed = speedRising ? 68 : 2;
  speedMultiplier = lerp(speedMultiplier, targetSpeed, dt * 3);
  if (speedRising && speedMultiplier >= 64.6) speedRising = false;
  if (!speedRising && speedMultiplier <= 2.4) speedRising = true;
  rotationX += dt * (8.5 * speedMultiplier);
  rotationY -= dt * (8.0 * speedMultiplier);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.lineWidth = 4;
  ctx.lineCap = "round";
  ctx.strokeStyle = "rgba(255, 255, 255, .96)";
  const vertices = [
    {x:-1,y:-1,z:-1}, {x:1,y:-1,z:-1}, {x:1,y:1,z:-1}, {x:-1,y:1,z:-1},
    {x:-1,y:-1,z:1}, {x:1,y:-1,z:1}, {x:1,y:1,z:1}, {x:-1,y:1,z:1}
  ];
  const scaleFactor = 1 + speedMultiplier * .0012;
  const projected = vertices.map((vertex) => {
    let rotated = rotateX(vertex, rotationX);
    rotated = rotateY(rotated, rotationY);
    rotated.x *= scaleFactor;
    rotated.y *= scaleFactor;
    rotated.z *= scaleFactor;
    return { x: 58 + rotated.x * 34 / 3, y: 56 + rotated.y * 34 / 3 };
  });
  const edges = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]];
  ctx.beginPath();
  for (const [a, b] of edges) {
    ctx.moveTo(projected[a].x, projected[a].y);
    ctx.lineTo(projected[b].x, projected[b].y);
  }
  ctx.stroke();
  requestAnimationFrame(drawCube);
}
function showOnly(panel) {
  notify.classList.remove("is-closing");
  settings.classList.remove("is-closing");
  notify.classList.toggle("is-visible", panel === notify);
  settings.classList.toggle("is-visible", panel === settings);
}
function resetProgress() {
  progress.style.animation = "none";
  progress.offsetHeight;
  progress.style.animation = "";
}
function showNotice() {
  clearTimeout(hideTimer);
  clearTimeout(closeTimer);
  title.textContent = "Image in clipboard";
  desc.textContent = "Click to save it as PNG.";
  tag.textContent = "READY";
  tag.style.color = "var(--accent)";
  resetProgress();
  showOnly(notify);
  hideTimer = setTimeout(() => {
    notify.classList.add("is-closing");
    notify.classList.remove("is-visible");
    closeTimer = setTimeout(() => window.chrome.webview.postMessage("hidden"), 190);
  }, 3000);
}
function showSettings(folder) {
  clearTimeout(hideTimer);
  clearTimeout(closeTimer);
  folderInput.value = folder || "";
  settingsStatus.textContent = "";
  settingsStatus.className = "status";
  showOnly(settings);
}
function setStatus(message, ok) {
  settingsStatus.textContent = message;
  settingsStatus.className = "status " + (ok ? "ok" : "bad");
}
function hsvToRgb(h, s, v) {
  const i = Math.floor(h * 6);
  const f = h * 6 - i;
  const p = v * (1 - s);
  const q = v * (1 - f * s);
  const t = v * (1 - (1 - f) * s);
  const table = [
    [v, t, p],
    [q, v, p],
    [p, v, t],
    [p, q, v],
    [t, p, v],
    [v, p, q],
  ];
  return table[i % 6].map((channel) => Math.round(Math.min(1, Math.max(0, channel)) * 255));
}
function rgbString([r, g, b]) {
  return `rgb(${r}, ${g}, ${b})`;
}
function rgbToHex([r, g, b]) {
  return "#" + [r, g, b].map((channel) => channel.toString(16).padStart(2, "0")).join("").toUpperCase();
}
function hexToRgb(color) {
  const normalized = /^#[0-9a-fA-F]{6}$/.test(color || "") ? color : "#B0B4FF";
  return [
    parseInt(normalized.slice(1, 3), 16),
    parseInt(normalized.slice(3, 5), 16),
    parseInt(normalized.slice(5, 7), 16),
  ];
}
function rgbToHsv([r, g, b]) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const delta = max - min;
  let h = 0;
  if (delta !== 0) {
    if (max === r) h = ((g - b) / delta) % 6;
    else if (max === g) h = (b - r) / delta + 2;
    else h = (r - g) / delta + 4;
    h /= 6;
    if (h < 0) h += 1;
  }
  return { h, s: max === 0 ? 0 : delta / max, v: max };
}
function currentAccentHex() {
  return rgbToHex(hsvToRgb(accentState.h, accentState.s, accentState.v));
}
function renderAccent() {
  const fullHue = rgbString(hsvToRgb(accentState.h, 1, 1));
  const current = rgbString(hsvToRgb(accentState.h, accentState.s, accentState.v));
  document.documentElement.style.setProperty("--accent", current);
  saturation.style.setProperty("--pos", `${accentState.s * 100}%`);
  brightness.style.setProperty("--pos", `${accentState.v * 100}%`);
  hue.style.setProperty("--pos", `${accentState.h * 100}%`);
  saturation.querySelector("span").style.background = `linear-gradient(90deg, #fff, ${fullHue})`;
  hue.querySelector("span").style.background = [
    "linear-gradient(90deg",
    rgbString(hsvToRgb(0, accentState.s, accentState.v)),
    rgbString(hsvToRgb(1 / 6, accentState.s, accentState.v)),
    rgbString(hsvToRgb(2 / 6, accentState.s, accentState.v)),
    rgbString(hsvToRgb(3 / 6, accentState.s, accentState.v)),
    rgbString(hsvToRgb(4 / 6, accentState.s, accentState.v)),
    rgbString(hsvToRgb(5 / 6, accentState.s, accentState.v)),
    rgbString(hsvToRgb(1, accentState.s, accentState.v)) + ")",
  ].join(", ");
}
function applyAccent(color) {
  const hsv = rgbToHsv(hexToRgb(color));
  accentState.h = hsv.h;
  accentState.s = hsv.s;
  accentState.v = hsv.v;
  renderAccent();
}
function applyAutoStart(enabled) {
  autoStartSwitch.classList.toggle("is-on", Boolean(enabled));
}
notify.addEventListener("click", () => {
  title.textContent = "Saving image";
  desc.textContent = "Writing PNG to the selected folder...";
  tag.textContent = "SAVE";
  window.chrome.webview.postMessage("save_clipboard_image");
});
document.getElementById("browseFolder").addEventListener("click", () => window.chrome.webview.postMessage("select_folder"));
document.getElementById("closeSettings").addEventListener("click", () => {
  settings.classList.add("is-closing");
  settings.classList.remove("is-visible");
  setTimeout(() => window.chrome.webview.postMessage("close_settings"), 180);
});
document.getElementById("exitApp").addEventListener("click", () => {
  settings.classList.add("is-closing");
  settings.classList.remove("is-visible");
  setTimeout(() => window.chrome.webview.postMessage("exit_app"), 180);
});
document.getElementById("saveFolder").addEventListener("click", () => {
  window.chrome.webview.postMessage("set_folder:" + folderInput.value.trim());
});
document.getElementById("openFolder").addEventListener("click", () => {
  window.chrome.webview.postMessage("open_folder");
});
)HTML" + LR"HTML(
autoStartControl.addEventListener("click", () => {
  const next = !autoStartSwitch.classList.contains("is-on");
  applyAutoStart(next);
  window.chrome.webview.postMessage("set_autostart:" + (next ? "1" : "0"));
});
[
  [saturation, "s"],
  [brightness, "v"],
  [hue, "h"],
].forEach(([control, key]) => {
  function setFromPointer(event) {
    const rect = control.getBoundingClientRect();
    accentState[key] = Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
    renderAccent();
  }
  control.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    control.setPointerCapture?.(event.pointerId);
    setFromPointer(event);
    const move = (moveEvent) => setFromPointer(moveEvent);
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      window.chrome.webview.postMessage("set_accent:" + currentAccentHex());
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  });
});
window.chrome.webview.addEventListener("message", (event) => {
  const msg = event.data;
  if (!msg) return;
  if (msg.type === "clipboard_image") {
    showNotice();
  } else if (msg.type === "settings") {
    showSettings(msg.folder);
    applyAccent(msg.accent);
    applyAutoStart(msg.autoStart);
  } else if (msg.type === "settings_state") {
    folderInput.value = msg.folder || "";
    applyAccent(msg.accent);
    applyAutoStart(msg.autoStart);
    setStatus(msg.message || "Settings saved", true);
  } else if (msg.type === "save_result") {
    if (msg.ok) {
      title.textContent = "Saved";
      desc.textContent = msg.path;
      tag.textContent = "OK";
      tag.style.color = "var(--ok)";
    } else {
      title.textContent = "Save failed";
      desc.textContent = "Set a valid folder and try again.";
      tag.textContent = "ERR";
      tag.style.color = "var(--bad)";
    }
  } else if (msg.type === "settings_error") {
    setStatus(msg.message || "Error", false);
  }
});
requestAnimationFrame(drawCube);
</script>
</body>
</html>)HTML";
}

void InitializeWebView() {
    const std::wstring html = BuildHtml();
    const std::wstring userDataPath = GetWebViewUserDataPath();

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataPath.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [html](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    return result;
                }

                environment->CreateCoreWebView2Controller(
                    g_window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [html](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller) {
                                return controllerResult;
                            }

                            g_webviewController = controller;
                            g_webviewController->get_CoreWebView2(&g_webview);

                            std::wstring assetsPath = GetWebCopyAssetsPath();
                            if (!assetsPath.empty()) {
                                wil::com_ptr<ICoreWebView2_3> webview3;
                                if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
                                    webview3->SetVirtualHostNameToFolderMapping(
                                        L"cringebufsaver.assets",
                                        assetsPath.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                }
                            }

                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(g_webviewController->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                COREWEBVIEW2_COLOR transparent = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(transparent);
                            }

                            wil::com_ptr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                            }

                            ResizeWebView();
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR rawMessage = nullptr;
                                        if (FAILED(args->TryGetWebMessageAsString(&rawMessage)) || !rawMessage) {
                                            return S_OK;
                                        }

                                        std::wstring message(rawMessage);
                                        CoTaskMemFree(rawMessage);

                                        if (message == L"hidden") {
                                            if (g_panelMode == PanelMode::Notify) {
                                                HidePanel();
                                            }
                                        }
                                        else if (message == L"save_clipboard_image") {
                                            std::wstring savedPath;
                                            bool saved = SaveClipboardImage(savedPath);
                                            if (saved) {
                                                CopySavedFileToClipboard(savedPath);
                                            }
                                            SendSaveResult(saved, savedPath);
                                        }
                                        else if (message == L"select_folder") {
                                            std::wstring folder;
                                            if (PickFolder(folder)) {
                                                g_settings.saveFolder = folder;
                                                SaveSettings();
                                                SendSettingsState(L"Folder saved");
                                                ShowSettingsPanel();
                                            }
                                        }
                                        else if (message == L"close_settings") {
                                            HidePanel();
                                        }
                                        else if (message == L"exit_app") {
                                            DestroyWindow(g_window);
                                        }
                                        else if (message == L"open_folder") {
                                            fs::create_directories(g_settings.saveFolder);
                                            ShellExecuteW(nullptr, L"open", g_settings.saveFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                        }
                                        else if (message.rfind(L"set_folder:", 0) == 0) {
                                            std::wstring folder = message.substr(11);
                                            if (folder.empty()) {
                                                PostJson(L"{\"type\":\"settings_error\",\"message\":\"Folder path is empty\"}");
                                            }
                                            else {
                                                g_settings.saveFolder = folder;
                                                std::error_code ignored;
                                                fs::create_directories(g_settings.saveFolder, ignored);
                                                SaveSettings();
                                                SendSettingsState(L"Folder saved");
                                            }
                                        }
                                        else if (message.rfind(L"set_autostart:", 0) == 0) {
                                            bool enabled = message.substr(14) == L"1";
                                            if (SetAutoStartEnabled(enabled)) {
                                                SendSettingsState(enabled ? L"Autostart enabled" : L"Autostart disabled");
                                            }
                                            else {
                                                g_settings.autoStart = fs::exists(GetStartupShortcutPath());
                                                SendSettingsState(L"Autostart change failed");
                                            }
                                        }
                                        else if (message.rfind(L"set_accent:", 0) == 0) {
                                            std::wstring color = message.substr(11);
                                            if (IsHexColor(color)) {
                                                g_settings.accentColor = color;
                                                SaveSettings();
                                                SendSettingsState(L"Accent saved");
                                            }
                                            else {
                                                PostJson(L"{\"type\":\"settings_error\",\"message\":\"Invalid color\"}");
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            g_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        g_webReady = true;
                                        if (g_pendingNotify.exchange(false)) {
                                            ShowNotify();
                                        }
                                        else if (g_showSettingsOnReady) {
                                            g_showSettingsOnReady = false;
                                            ShowSettingsPanel();
                                        }
                                        else {
                                            HidePanel();
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            g_webview->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        g_pngClipboardFormat = RegisterClipboardFormatW(L"PNG");
        g_lastClipboardSequence = GetClipboardSequenceNumber();
        AddClipboardFormatListener(window);
        if (!RegisterHotKey(window, kHotkeySettings, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_SPACE)) {
            RegisterHotKey(window, kHotkeySettings, MOD_CONTROL | MOD_SHIFT, VK_SPACE);
        }
        InitializeWebView();
        return 0;

    case WM_CLIPBOARDUPDATE:
        HandleClipboardUpdate();
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeySettings) {
            ShowSettingsPanel();
            return 0;
        }
        break;

    case kShowSettingsMessage:
        ShowSettingsPanel();
        return 0;

    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_TIMER:
        if (wParam == kHideTimerId && g_panelMode == PanelMode::Notify) {
            HidePanel();
            return 0;
        }
        if (wParam == kClipboardRetryTimerId) {
            KillTimer(g_window, kClipboardRetryTimerId);
            CheckClipboardForImage();
            return 0;
        }
        if (wParam == kSavedHideTimerId) {
            HidePanel();
            return 0;
        }
        break;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (g_windowVisible) {
            const bool settings = g_panelMode == PanelMode::Settings;
            MovePanelToBottomRight(settings ? kSettingsWidth : kNotifyWidth, settings ? kSettingsHeight : kNotifyHeight);
        }
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(window, kHotkeySettings);
        RemoveClipboardFormatListener(window);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWindow = FindWindowW(kWindowClass, L"CringeBufSaverDaemon");
        if (existingWindow) {
            PostMessageW(existingWindow, kShowSettingsMessage, 0, 0);
        }
        if (singleInstance) {
            CloseHandle(singleInstance);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        CloseHandle(singleInstance);
        return 1;
    }

    LoadSettings();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    g_targetMonitor = MonitorFromCurrentCursor();
    RECT workArea = GetMonitorWorkArea(g_targetMonitor);
    g_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"CringeBufSaverDaemon",
        WS_POPUP,
        workArea.right - kNotifyWidth - kScreenPadding,
        workArea.bottom - kNotifyHeight - kScreenPadding,
        kNotifyWidth,
        kNotifyHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_window) {
        CoUninitialize();
        CloseHandle(singleInstance);
        return 1;
    }

    SetLayeredWindowAttributes(g_window, 0, 255, LWA_ALPHA);
    const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_window, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));

    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    g_windowVisible = true;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_webview = nullptr;
    g_webviewController = nullptr;
    CoUninitialize();
    CloseHandle(singleInstance);
    return static_cast<int>(message.wParam);
}
