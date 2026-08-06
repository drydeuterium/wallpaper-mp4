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

constexpr wchar_t kWindowClassName[] = L"WallpaperMp4Window";
constexpr wchar_t kApplicationName[] = L"wallpaper-mp4";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kPlaybackEndedMessage = WM_APP + 2;
constexpr UINT kPositionSetMessage = WM_APP + 3;
constexpr UINT kPlaybackErrorMessage = WM_APP + 4;
constexpr UINT kMediaItemSetMessage = WM_APP + 5;

constexpr UINT_PTR kOpenCommand = 1001;
constexpr UINT_PTR kPauseCommand = 1002;
constexpr UINT_PTR kExitCommand = 1003;

constexpr UINT kExplorerSpawnWorkerMessage = 0x052C;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_wallpaperHost = nullptr;
IMFPMediaPlayer* g_player = nullptr;
NOTIFYICONDATAW g_trayIcon{};
UINT g_taskbarCreatedMessage = 0;
bool g_restartPending = false;
std::wstring g_currentPath;

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
    MessageBoxW(nullptr, completeMessage.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
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

std::wstring PickMp4File() {
    std::vector<wchar_t> fileName(32768);
    constexpr wchar_t filter[] = L"MP4 video (*.mp4)\0*.mp4\0All files (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
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
    if (host == nullptr || g_window == nullptr) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(g_window, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(g_window, GWL_STYLE, style);

    SetLastError(ERROR_SUCCESS);
    const HWND previousParent = SetParent(g_window, host);
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
        g_window,
        WallpaperInsertAfter(host),
        corners[0].x,
        corners[0].y,
        corners[1].x - corners[0].x,
        corners[1].y - corners[0].y,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void ResizeForPrimaryMonitor() {
    if (g_window == nullptr || g_wallpaperHost == nullptr || !IsWindow(g_wallpaperHost)) {
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
        g_window,
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
        g_window,
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

void OpenNewVideo() {
    const std::wstring path = PickMp4File();
    if (path.empty()) {
        return;
    }

    const HRESULT result = StartPlayback(path);
    if (result == MF_E_INVALIDMEDIATYPE) {
        ShowError(L"H.264映像を含むMP4だけを再生できる。");
    } else if (FAILED(result)) {
        ShowError(L"MP4を開けなかった。", result);
    }
}

void AddTrayIcon() {
    g_trayIcon = {};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_window;
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

    AppendMenuW(menu, MF_STRING, kOpenCommand, L"MP4を開く...");
    AppendMenuW(menu, MF_STRING | (g_player == nullptr ? MF_GRAYED : 0), kPauseCommand, pauseLabel);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"終了");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        g_window,
        nullptr);
    DestroyMenu(menu);

    switch (command) {
    case kOpenCommand:
        OpenNewVideo();
        break;
    case kPauseCommand:
        TogglePlayback();
        break;
    case kExitCommand:
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        AddTrayIcon();
        AttachToDesktop();
        return 0;
    }

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
    case WM_DISPLAYCHANGE:
        ResizeForPrimaryMonitor();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
            ShowTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            TogglePlayback();
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
                ShowError(L"動画の再生を開始できなかった。", result);
            } else {
                SaveCurrentPath(g_currentPath);
            }
        }
        return 0;
    case kPlaybackErrorMessage:
        if (reinterpret_cast<IMFPMediaPlayer*>(wParam) == g_player) {
            ShowError(L"再生中にMedia Foundationエラーが発生した。", static_cast<HRESULT>(lParam));
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_callback != nullptr) {
            g_callback->SetActivePlayer(nullptr);
            g_callback->ClearWindow();
        }
        ShutdownPlayer(g_player);
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

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
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

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        ShowError(L"ウィンドウクラスを登録できなかった。", HRESULT_FROM_WIN32(GetLastError()));
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    g_window = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kWindowClassName,
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
    if (g_window == nullptr) {
        ShowError(L"壁紙ウィンドウを作成できなかった。", HRESULT_FROM_WIN32(GetLastError()));
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    g_callback = new MediaPlayerCallback(g_window);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    std::wstring initialPath = CommandLineVideoPath();
    if (initialPath.empty()) {
        initialPath = LoadCurrentPath();
    }
    if (!IsExistingFile(initialPath)) {
        initialPath = PickMp4File();
    }

    if (initialPath.empty()) {
        DestroyWindow(g_window);
    } else {
        result = StartPlayback(initialPath);
        if (result == MF_E_INVALIDMEDIATYPE) {
            ShowError(L"H.264映像を含むMP4だけを再生できる。");
        } else if (FAILED(result)) {
            ShowError(L"MP4を開けなかった。", result);
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
