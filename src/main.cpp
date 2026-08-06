#include <windows.h>

#include <commdlg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kControlWindowClassName[] = L"WallpaperMp4ControlWindow";
constexpr wchar_t kWallpaperWindowClassName[] = L"WallpaperMp4Window";
constexpr wchar_t kApplicationName[] = L"wallpaper-mp4";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kPlaybackEndedMessage = WM_APP + 2;
constexpr UINT kPositionSetMessage = WM_APP + 3;
constexpr UINT kPlaybackErrorMessage = WM_APP + 4;
constexpr UINT kMediaItemSetMessage = WM_APP + 5;
constexpr UINT kPlaybackStateMessage = WM_APP + 6;

constexpr UINT_PTR kShowCommand = 1001;
constexpr UINT_PTR kOpenCommand = 1002;
constexpr UINT_PTR kPauseCommand = 1003;
constexpr UINT_PTR kExitCommand = 1004;

constexpr int kPathEditId = 2001;
constexpr int kBrowseButtonId = 2002;
constexpr int kApplyButtonId = 2003;
constexpr int kPauseButtonId = 2004;
constexpr int kExitButtonId = 2005;

constexpr UINT kExplorerSpawnWorkerMessage = 0x052C;

HINSTANCE g_instance = nullptr;
HWND g_controlWindow = nullptr;
HWND g_wallpaperWindow = nullptr;
HWND g_wallpaperHost = nullptr;
HWND g_pathEdit = nullptr;
HWND g_statusLabel = nullptr;
HWND g_pauseButton = nullptr;
IMFPMediaPlayer* g_player = nullptr;
NOTIFYICONDATAW g_trayIcon{};
UINT g_taskbarCreatedMessage = 0;
bool g_restartPending = false;
bool g_isExiting = false;
std::wstring g_currentPath;
std::wstring g_selectedPath;

void ShowError(const std::wstring& message, HRESULT result = S_OK);

template <typename T>
void SafeRelease(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

class MediaPlayerCallback final : public IMFPMediaPlayerCallback {
public:
    explicit MediaPlayerCallback(HWND window) : window_(window) {}

    void SetActivePlayer(IMFPMediaPlayer* player) {
        activePlayer_.store(player, std::memory_order_release);
    }

    void ClearWindow() {
        window_.store(nullptr, std::memory_order_release);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (interfaceId == IID_IUnknown || interfaceId == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&referenceCount_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override {
        if (eventHeader == nullptr ||
            eventHeader->pMediaPlayer != activePlayer_.load(std::memory_order_acquire)) {
            return;
        }

        const HWND window = window_.load(std::memory_order_acquire);
        if (window == nullptr) {
            return;
        }

        if (FAILED(eventHeader->hrEvent)) {
            PostMessageW(
                window,
                kPlaybackErrorMessage,
                reinterpret_cast<WPARAM>(eventHeader->pMediaPlayer),
                static_cast<LPARAM>(eventHeader->hrEvent));
            return;
        }

        switch (eventHeader->eEventType) {
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            PostMessageW(
                window,
                kPlaybackEndedMessage,
                reinterpret_cast<WPARAM>(eventHeader->pMediaPlayer),
                0);
            break;
        case MFP_EVENT_TYPE_POSITION_SET:
            PostMessageW(
                window,
                kPositionSetMessage,
                reinterpret_cast<WPARAM>(eventHeader->pMediaPlayer),
                0);
            break;
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
            PostMessageW(
                window,
                kMediaItemSetMessage,
                reinterpret_cast<WPARAM>(eventHeader->pMediaPlayer),
                0);
            break;
        case MFP_EVENT_TYPE_PLAY:
        case MFP_EVENT_TYPE_PAUSE:
        case MFP_EVENT_TYPE_STOP:
            PostMessageW(
                window,
                kPlaybackStateMessage,
                reinterpret_cast<WPARAM>(eventHeader->pMediaPlayer),
                0);
            break;
        default:
            break;
        }
    }

private:
    ~MediaPlayerCallback() = default;

    volatile LONG referenceCount_ = 1;
    std::atomic<HWND> window_;
    std::atomic<IMFPMediaPlayer*> activePlayer_ = nullptr;
};

MediaPlayerCallback* g_callback = nullptr;

std::wstring FormatSystemMessage(HRESULT result) {
    wchar_t* rawMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&rawMessage),
        0,
        nullptr);

    std::wstring message;
    if (length != 0 && rawMessage != nullptr) {
        message.assign(rawMessage, length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    LocalFree(rawMessage);

    if (message.empty()) {
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
        message = code;
    }
    return message;
}

void ShowError(const std::wstring& message, HRESULT result) {
    std::wstring completeMessage = message;
    if (FAILED(result)) {
        completeMessage += L"\n\n";
        completeMessage += FormatSystemMessage(result);
    }
    MessageBoxW(g_controlWindow, completeMessage.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
}

std::filesystem::path SettingsPath() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) {
        return {};
    }

    std::filesystem::path directory = std::filesystem::path(localAppData) / kApplicationName;
    CoTaskMemFree(localAppData);

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return {};
    }
    return directory / L"settings.ini";
}

void SaveCurrentPath(const std::wstring& path) {
    const std::filesystem::path settings = SettingsPath();
    if (!settings.empty()) {
        WritePrivateProfileStringW(L"playback", L"path", path.c_str(), settings.c_str());
    }
}

std::wstring LoadCurrentPath() {
    const std::filesystem::path settings = SettingsPath();
    if (settings.empty()) {
        return {};
    }

    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(
        L"playback",
        L"path",
        L"",
        value.data(),
        static_cast<DWORD>(value.size()),
        settings.c_str());
    return value.data();
}

bool IsMp4Path(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return extension == L".mp4";
}

bool IsExistingFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring PickMp4File(HWND owner) {
    std::vector<wchar_t> fileName(32768);
    constexpr wchar_t filter[] = L"MP4 video (*.mp4)\0*.mp4\0All files (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileName.data();
    dialog.nMaxFile = static_cast<DWORD>(fileName.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"mp4";

    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }
    return fileName.data();
}

BOOL CALLBACK FindWallpaperWorker(HWND topLevelWindow, LPARAM parameter) {
    if (FindWindowExW(topLevelWindow, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) {
        return TRUE;
    }

    auto* result = reinterpret_cast<HWND*>(parameter);
    *result = FindWindowExW(nullptr, topLevelWindow, L"WorkerW", nullptr);
    return *result == nullptr;
}

HWND FindWallpaperHost() {
    HWND programManager = FindWindowW(L"Progman", L"Program Manager");
    if (programManager == nullptr) {
        programManager = FindWindowW(L"Progman", nullptr);
    }
    if (programManager == nullptr) {
        return nullptr;
    }

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(
        programManager,
        kExplorerSpawnWorkerMessage,
        0,
        0,
        SMTO_NORMAL,
        1000,
        &ignored);

    HWND worker = nullptr;
    EnumWindows(FindWallpaperWorker, reinterpret_cast<LPARAM>(&worker));
    return worker != nullptr ? worker : programManager;
}

RECT PrimaryMonitorRectangle() {
    POINT origin{};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information)) {
        return information.rcMonitor;
    }
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

HWND WallpaperInsertAfter(HWND host) {
    const HWND iconView = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
    return iconView != nullptr ? iconView : HWND_TOP;
}

bool AttachToDesktop() {
    HWND host = FindWallpaperHost();
    if (host == nullptr || g_wallpaperWindow == nullptr) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(g_wallpaperWindow, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(g_wallpaperWindow, GWL_STYLE, style);

    SetLastError(ERROR_SUCCESS);
    const HWND previousParent = SetParent(g_wallpaperWindow, host);
    if (previousParent == nullptr && GetLastError() != ERROR_SUCCESS) {
        return false;
    }

    RECT monitor = PrimaryMonitorRectangle();
    POINT corners[2] = {
        {monitor.left, monitor.top},
        {monitor.right, monitor.bottom},
    };
    MapWindowPoints(HWND_DESKTOP, host, corners, 2);

    g_wallpaperHost = host;
    SetWindowPos(
        g_wallpaperWindow,
        WallpaperInsertAfter(host),
        corners[0].x,
        corners[0].y,
        corners[1].x - corners[0].x,
        corners[1].y - corners[0].y,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void ResizeForPrimaryMonitor() {
    if (g_wallpaperWindow == nullptr || g_wallpaperHost == nullptr || !IsWindow(g_wallpaperHost)) {
        AttachToDesktop();
        return;
    }

    RECT monitor = PrimaryMonitorRectangle();
    POINT corners[2] = {
        {monitor.left, monitor.top},
        {monitor.right, monitor.bottom},
    };
    MapWindowPoints(HWND_DESKTOP, g_wallpaperHost, corners, 2);
    SetWindowPos(
        g_wallpaperWindow,
        WallpaperInsertAfter(g_wallpaperHost),
        corners[0].x,
        corners[0].y,
        corners[1].x - corners[0].x,
        corners[1].y - corners[0].y,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

HRESULT InspectMediaItem(IMFPMediaItem* item) {
    DWORD streamCount = 0;
    HRESULT result = item->GetNumberOfStreams(&streamCount);
    if (FAILED(result)) {
        return result;
    }

    bool selectedH264Video = false;
    for (DWORD index = 0; index < streamCount; ++index) {
        BOOL selected = FALSE;
        if (FAILED(item->GetStreamSelection(index, &selected)) || !selected) {
            continue;
        }

        PROPVARIANT majorType{};
        PropVariantInit(&majorType);
        result = item->GetStreamAttribute(index, MF_MT_MAJOR_TYPE, &majorType);
        if (FAILED(result) || majorType.vt != VT_CLSID || majorType.puuid == nullptr) {
            PropVariantClear(&majorType);
            continue;
        }

        if (*majorType.puuid == MFMediaType_Audio) {
            const HRESULT disableResult = item->SetStreamSelection(index, FALSE);
            if (FAILED(disableResult)) {
                PropVariantClear(&majorType);
                return disableResult;
            }
        } else if (*majorType.puuid == MFMediaType_Video) {
            PROPVARIANT subtype{};
            PropVariantInit(&subtype);
            if (SUCCEEDED(item->GetStreamAttribute(index, MF_MT_SUBTYPE, &subtype)) &&
                subtype.vt == VT_CLSID &&
                subtype.puuid != nullptr &&
                (*subtype.puuid == MFVideoFormat_H264 || *subtype.puuid == MFVideoFormat_H264_ES)) {
                selectedH264Video = true;
            }
            PropVariantClear(&subtype);
        }

        PropVariantClear(&majorType);
    }

    return selectedH264Video ? S_OK : MF_E_INVALIDMEDIATYPE;
}

void ShutdownPlayer(IMFPMediaPlayer*& player) {
    if (player != nullptr) {
        player->Shutdown();
        SafeRelease(player);
    }
}

HRESULT StartPlayback(const std::wstring& path) {
    if (!IsExistingFile(path)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (!IsMp4Path(path)) {
        return MF_E_INVALIDMEDIATYPE;
    }

    IMFPMediaPlayer* nextPlayer = nullptr;
    IMFPMediaItem* mediaItem = nullptr;

    HRESULT result = MFPCreateMediaPlayer(
        nullptr,
        FALSE,
        MFP_OPTION_NONE,
        g_callback,
        g_wallpaperWindow,
        &nextPlayer);
    if (SUCCEEDED(result)) {
        result = nextPlayer->CreateMediaItemFromURL(path.c_str(), TRUE, 0, &mediaItem);
    }
    if (SUCCEEDED(result)) {
        result = InspectMediaItem(mediaItem);
    }
    IMFPMediaPlayer* previousPlayer = g_player;
    if (SUCCEEDED(result)) {
        g_callback->SetActivePlayer(nextPlayer);
        g_player = nextPlayer;
        result = nextPlayer->SetMediaItem(mediaItem);
    }
    SafeRelease(mediaItem);

    if (FAILED(result)) {
        if (g_player == nextPlayer) {
            g_player = previousPlayer;
            g_callback->SetActivePlayer(previousPlayer);
        }
        ShutdownPlayer(nextPlayer);
        return result;
    }

    ShutdownPlayer(previousPlayer);
    g_currentPath = path;
    g_restartPending = false;

    if (!AttachToDesktop()) {
        const DWORD error = GetLastError() == ERROR_SUCCESS ? ERROR_INVALID_WINDOW_HANDLE : GetLastError();
        g_callback->SetActivePlayer(nullptr);
        ShutdownPlayer(g_player);
        return HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

void SetStatus(const wchar_t* status) {
    if (g_statusLabel != nullptr) {
        SetWindowTextW(g_statusLabel, status);
    }
}

void UpdateControls() {
    if (g_pathEdit != nullptr) {
        SetWindowTextW(g_pathEdit, g_selectedPath.c_str());
    }

    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (g_player != nullptr) {
        g_player->GetState(&state);
    }

    if (g_pauseButton != nullptr) {
        EnableWindow(g_pauseButton, g_player != nullptr);
        SetWindowTextW(
            g_pauseButton,
            state == MFP_MEDIAPLAYER_STATE_PLAYING ? L"一時停止" : L"再生");
    }

    if (g_player == nullptr) {
        SetStatus(L"状態: 動画が設定されていない");
    } else if (state == MFP_MEDIAPLAYER_STATE_PLAYING) {
        SetStatus(L"状態: 再生中");
    } else if (state == MFP_MEDIAPLAYER_STATE_PAUSED) {
        SetStatus(L"状態: 一時停止中");
    } else {
        SetStatus(L"状態: 読み込み中");
    }
}

void ShowControlWindow() {
    if (g_controlWindow == nullptr) {
        return;
    }

    ShowWindow(g_controlWindow, IsIconic(g_controlWindow) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_controlWindow);
}

void SetSelectedPath(const std::wstring& path) {
    g_selectedPath = path;
    if (g_pathEdit != nullptr) {
        SetWindowTextW(g_pathEdit, path.c_str());
    }
}

void ApplySelectedVideo() {
    if (g_selectedPath.empty()) {
        ShowError(L"先にH.264 MP4を選択すること。");
        SetStatus(L"状態: MP4が選択されていない");
        return;
    }

    SetStatus(L"状態: 読み込み中...");
    const HRESULT result = StartPlayback(g_selectedPath);
    if (result == MF_E_INVALIDMEDIATYPE) {
        SetStatus(L"状態: H.264 MP4ではない");
        ShowError(L"H.264映像を含むMP4だけを再生できる。");
    } else if (FAILED(result)) {
        SetStatus(L"状態: MP4を開けなかった");
        ShowError(L"MP4を開けなかった。", result);
    }
}

void SelectVideo(bool applyImmediately) {
    const std::wstring path = PickMp4File(g_controlWindow);
    if (path.empty()) {
        return;
    }

    SetSelectedPath(path);
    if (applyImmediately) {
        ApplySelectedVideo();
    } else {
        SetStatus(L"状態: 「壁紙に設定」を押すと再生を開始する");
    }
}

bool CreateControlChildren(HWND window) {
    constexpr DWORD labelStyle = WS_CHILD | WS_VISIBLE;
    constexpr DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

    HWND pathLabel = CreateWindowExW(
        0, L"STATIC", L"H.264 MP4ファイル", labelStyle,
        20, 18, 300, 20, window, nullptr, g_instance, nullptr);
    g_pathEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        20, 42, 480, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)), g_instance, nullptr);
    HWND browseButton = CreateWindowExW(
        0, L"BUTTON", L"参照...", buttonStyle,
        510, 41, 96, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseButtonId)), g_instance, nullptr);
    HWND applyButton = CreateWindowExW(
        0, L"BUTTON", L"壁紙に設定", buttonStyle,
        20, 86, 140, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButtonId)), g_instance, nullptr);
    g_pauseButton = CreateWindowExW(
        0, L"BUTTON", L"再生", buttonStyle,
        170, 86, 110, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPauseButtonId)), g_instance, nullptr);
    HWND exitButton = CreateWindowExW(
        0, L"BUTTON", L"終了", buttonStyle,
        290, 86, 90, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExitButtonId)), g_instance, nullptr);
    g_statusLabel = CreateWindowExW(
        0, L"STATIC", L"状態: 動画が設定されていない", labelStyle,
        20, 138, 586, 22, window, nullptr, g_instance, nullptr);
    HWND hintLabel = CreateWindowExW(
        0, L"STATIC", L"最小化または×でタスクトレイに格納する。", labelStyle,
        20, 166, 586, 22, window, nullptr, g_instance, nullptr);

    const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const HWND controls[] = {
        pathLabel,
        g_pathEdit,
        browseButton,
        applyButton,
        g_pauseButton,
        exitButton,
        g_statusLabel,
        hintLabel,
    };
    for (HWND control : controls) {
        if (control == nullptr) {
            return false;
        }
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    UpdateControls();
    return true;
}

void RestartPlayback() {
    if (g_player == nullptr) {
        return;
    }

    PROPVARIANT position{};
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;
    g_restartPending = true;
    const HRESULT result = g_player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    PropVariantClear(&position);

    if (FAILED(result)) {
        g_restartPending = false;
        ShowError(L"動画を先頭へ戻せなかった。", result);
    }
}

void TogglePlayback() {
    if (g_player == nullptr) {
        return;
    }

    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (FAILED(g_player->GetState(&state))) {
        return;
    }

    if (state == MFP_MEDIAPLAYER_STATE_PLAYING) {
        g_player->Pause();
    } else if (state == MFP_MEDIAPLAYER_STATE_PAUSED || state == MFP_MEDIAPLAYER_STATE_STOPPED) {
        g_player->Play();
    }
}

void AddTrayIcon() {
    g_trayIcon = {};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_controlWindow;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, kApplicationName);
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_trayIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_trayIcon);
}

void RemoveTrayIcon() {
    if (g_trayIcon.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        g_trayIcon = {};
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (g_player != nullptr) {
        g_player->GetState(&state);
    }
    const wchar_t* pauseLabel = state == MFP_MEDIAPLAYER_STATE_PLAYING ? L"一時停止" : L"再生";

    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kShowCommand, L"設定を開く");
    AppendMenuW(menu, MF_STRING, kOpenCommand, L"MP4を選択...");
    AppendMenuW(menu, MF_STRING | (g_player == nullptr ? MF_GRAYED : 0), kPauseCommand, pauseLabel);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"終了");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_controlWindow);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        g_controlWindow,
        nullptr);
    DestroyMenu(menu);

    switch (command) {
    case kShowCommand:
        ShowControlWindow();
        break;
    case kOpenCommand:
        SelectVideo(true);
        break;
    case kPauseCommand:
        TogglePlayback();
        break;
    case kExitCommand:
        g_isExiting = true;
        DestroyWindow(g_controlWindow);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WallpaperWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (g_player != nullptr) {
            g_player->UpdateVideo();
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        if (g_player != nullptr) {
            g_player->UpdateVideo();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK ControlWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        AddTrayIcon();
        if (g_player != nullptr) {
            AttachToDesktop();
        }
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        if (!CreateControlChildren(window)) {
            return -1;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kBrowseButtonId:
            SelectVideo(false);
            break;
        case kApplyButtonId:
            ApplySelectedVideo();
            break;
        case kPauseButtonId:
            TogglePlayback();
            break;
        case kExitButtonId:
            g_isExiting = true;
            DestroyWindow(window);
            break;
        default:
            break;
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_player != nullptr) {
            ResizeForPrimaryMonitor();
        }
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
            ShowTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowControlWindow();
        }
        return 0;
    case kPlaybackEndedMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player) {
            RestartPlayback();
        }
        return 0;
    case kPositionSetMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player && g_restartPending) {
            g_restartPending = false;
            const HRESULT result = g_player->Play();
            if (FAILED(result)) {
                SetStatus(L"状態: ループ再生を再開できなかった");
                ShowError(L"ループ再生を再開できなかった。", result);
            }
        }
        return 0;
    case kMediaItemSetMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player) {
            HRESULT result = g_player->SetMute(TRUE);
            if (SUCCEEDED(result)) {
                result = g_player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
            }
            if (SUCCEEDED(result)) {
                result = g_player->SetBorderColor(RGB(0, 0, 0));
            }
            if (SUCCEEDED(result)) {
                result = g_player->Play();
            }
            if (FAILED(result)) {
                SetStatus(L"状態: 動画の再生を開始できなかった");
                ShowError(L"動画の再生を開始できなかった。", result);
            } else {
                SaveCurrentPath(g_currentPath);
                UpdateControls();
            }
        }
        return 0;
    case kPlaybackStateMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player) {
            UpdateControls();
        }
        return 0;
    case kPlaybackErrorMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player) {
            SetStatus(L"状態: 再生エラー");
            ShowError(L"再生中にMedia Foundationエラーが発生した。", static_cast<HRESULT>(lParam));
        }
        return 0;
    case WM_CLOSE:
        if (g_isExiting) {
            DestroyWindow(window);
        } else {
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_callback != nullptr) {
            g_callback->SetActivePlayer(nullptr);
            g_callback->ClearWindow();
        }
        ShutdownPlayer(g_player);
        if (g_wallpaperWindow != nullptr) {
            DestroyWindow(g_wallpaperWindow);
            g_wallpaperWindow = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

std::wstring CommandLineVideoPath() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return {};
    }

    std::wstring path;
    if (argumentCount >= 2) {
        path = arguments[1];
    }
    LocalFree(arguments);
    return path;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int showCommand) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) {
        ShowError(L"COMを初期化できなかった。", result);
        return 1;
    }

    result = MFStartup(MF_VERSION);
    if (FAILED(result)) {
        ShowError(L"Media Foundationを初期化できなかった。", result);
        CoUninitialize();
        return 1;
    }

    WNDCLASSEXW wallpaperClass{};
    wallpaperClass.cbSize = sizeof(wallpaperClass);
    wallpaperClass.lpfnWndProc = WallpaperWindowProcedure;
    wallpaperClass.hInstance = instance;
    wallpaperClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wallpaperClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wallpaperClass.lpszClassName = kWallpaperWindowClassName;

    WNDCLASSEXW controlClass{};
    controlClass.cbSize = sizeof(controlClass);
    controlClass.lpfnWndProc = ControlWindowProcedure;
    controlClass.hInstance = instance;
    controlClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    controlClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    controlClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    controlClass.lpszClassName = kControlWindowClassName;

    if (RegisterClassExW(&wallpaperClass) == 0 || RegisterClassExW(&controlClass) == 0) {
        ShowError(L"ウィンドウクラスを登録できなかった。", HRESULT_FROM_WIN32(GetLastError()));
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    g_wallpaperWindow = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kWallpaperWindowClassName,
        kApplicationName,
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (g_wallpaperWindow == nullptr) {
        ShowError(L"壁紙ウィンドウを作成できなかった。", HRESULT_FROM_WIN32(GetLastError()));
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    constexpr DWORD controlStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT controlRectangle{0, 0, 630, 205};
    AdjustWindowRectEx(&controlRectangle, controlStyle, FALSE, 0);
    g_controlWindow = CreateWindowExW(
        0,
        kControlWindowClassName,
        kApplicationName,
        controlStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        controlRectangle.right - controlRectangle.left,
        controlRectangle.bottom - controlRectangle.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (g_controlWindow == nullptr) {
        ShowError(L"設定ウィンドウを作成できなかった。", HRESULT_FROM_WIN32(GetLastError()));
        DestroyWindow(g_wallpaperWindow);
        g_wallpaperWindow = nullptr;
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    g_callback = new MediaPlayerCallback(g_controlWindow);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    ShowWindow(g_controlWindow, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(g_controlWindow);

    std::wstring initialPath = CommandLineVideoPath();
    if (initialPath.empty()) {
        initialPath = LoadCurrentPath();
    }
    if (!initialPath.empty()) {
        SetSelectedPath(initialPath);
        if (IsExistingFile(initialPath)) {
            ApplySelectedVideo();
        } else {
            SetStatus(L"状態: 前回のMP4が見つからない");
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_callback != nullptr) {
        g_callback->Release();
        g_callback = nullptr;
    }
    MFShutdown();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
