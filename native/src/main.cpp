#include <amp/media.hpp>
#include <amp/service.hpp>
#include <amp/settings.hpp>
#include <Windows.h>
#include <Shellapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr UINT tray_message = WM_APP + 1, status_message = WM_APP + 2;
constexpr UINT id_settings = 101, id_pause = 102, id_exit = 103, id_save = 104;
constexpr UINT id_artwork = 105, id_motion = 106;
constexpr wchar_t window_class[] = L"AppleMusicPresence.Native.Window";
constexpr wchar_t startup_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t startup_name[] = L"AppleMusicPresence.Native";
struct StartupValue { DWORD type{}; std::vector<BYTE> data; };
std::optional<StartupValue> startup_value() {
    DWORD size{}, type{};
    auto result = RegGetValueW(HKEY_CURRENT_USER, startup_key, startup_name, RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) return {};
    if (result != ERROR_SUCCESS || size > 65536) throw std::runtime_error("Could not read automatic startup");
    StartupValue value{type, std::vector<BYTE>(size)};
    if (RegGetValueW(HKEY_CURRENT_USER, startup_key, startup_name, RRF_RT_ANY | RRF_NOEXPAND, &value.type, value.data.data(), &size) != ERROR_SUCCESS)
        throw std::runtime_error("Could not read automatic startup");
    value.data.resize(size); return value;
}
void restore_startup(const std::optional<StartupValue>& value) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, startup_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not restore automatic startup");
    auto result = value ? RegSetValueExW(key, startup_name, 0, value->type, value->data.data(), static_cast<DWORD>(value->data.size()))
                        : RegDeleteValueW(key, startup_name);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) throw std::runtime_error("Could not restore automatic startup");
}
std::wstring executable() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size == path.size()) throw std::runtime_error("Could not locate the app executable");
    path.resize(size); return path;
}
bool startup_enabled() {
    wchar_t value[32768]{}; DWORD size = sizeof(value);
    auto result = RegGetValueW(HKEY_CURRENT_USER, startup_key, startup_name, RRF_RT_REG_SZ, nullptr, value, &size);
    return result == ERROR_SUCCESS && std::wstring(value) == L"\"" + executable() + L"\"";
}
void set_startup(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, startup_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Windows could not update automatic startup");
    std::wstring command = L"\"" + executable() + L"\"";
    auto result = enabled ? RegSetValueExW(key, startup_name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) : RegDeleteValueW(key, startup_name);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && !(result == ERROR_FILE_NOT_FOUND && !enabled))
        throw std::runtime_error("Windows could not update automatic startup");
}
std::wstring control_text(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1); text.resize(length);
    auto first = text.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
HICON music_icon() {
    HDC dc = GetDC(nullptr), memory = CreateCompatibleDC(dc);
    HBITMAP color = CreateCompatibleBitmap(dc, 32, 32), mask = CreateBitmap(32, 32, 1, 1, nullptr);
    auto old = SelectObject(memory, color);
    RECT bounds{0, 0, 32, 32}; FillRect(memory, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HFONT font = CreateFontW(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
    auto old_font = SelectObject(memory, font); SetTextColor(memory, RGB(255, 255, 255)); SetBkMode(memory, TRANSPARENT);
    DrawTextW(memory, L"\x266b", 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memory, old_font); DeleteObject(font); SelectObject(memory, mask);
    FillRect(memory, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); SelectObject(memory, old);
    ICONINFO info{TRUE, 0, 0, mask, color}; HICON icon = CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask); DeleteDC(memory); ReleaseDC(nullptr, dc);
    return icon;
}
class TrayApp {
public:
    TrayApp(amp::Settings settings, std::filesystem::path directory, bool demo, double seconds)
        : settings_(std::move(settings)), directory_(std::move(directory)), demo_(demo), seconds_(seconds) {}
    ~TrayApp() { service_.reset(); remove_tray(); if (icon_) DestroyIcon(icon_); if (font_) DeleteObject(font_); }
    int run(HINSTANCE instance) {
        WNDCLASSW type{}; type.lpfnWndProc = dispatch; type.hInstance = instance;
        type.lpszClassName = window_class; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&type);
        window_ = CreateWindowExW(0, window_class, demo_ ? L"Apple Music Presence — Offline preview" : L"Apple Music Presence",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 500, 445, nullptr, nullptr, instance, this);
        if (!window_) throw std::runtime_error("Windows could not create the settings window");
        icon_ = music_icon(); SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
        build_controls(); add_tray(); taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        service_ = std::make_unique<amp::Service>(settings_, directory_, [this](const amp::Status& status) {
            bool changed;
            { std::lock_guard lock(mutex_); changed = status.message != status_.message || status.snapshot.track != status_.snapshot.track
                  || status.snapshot.state != status_.snapshot.state || status.artwork_status != status_.artwork_status;
              status_ = status; }
            if (changed) PostMessageW(window_, status_message, 0, 0);
        }, demo_);
        if (demo_ || settings_.client_id.empty()) show();
        else { try { amp::validate_settings(settings_); } catch (...) { show(); } }
        if (seconds_ > 0) SetTimer(window_, 2, static_cast<UINT>(seconds_ * 1000), nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        service_.reset(); return 0;
    }
private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, UINT id = 0) {
        HWND result = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text,
            WS_CHILD | WS_VISIBLE | style, x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), nullptr, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return result;
    }
    void build_controls() {
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        control(L"STATIC", L"Discord Application ID", 0, 20, 16, 300, 24);
        client_ = control(L"EDIT", amp::wide(settings_.client_id).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 20, 44, 445, 26);
        SendMessageW(client_, EM_SETLIMITTEXT, 20, 0);
        control(L"STATIC", L"Apple storefront", 0, 20, 82, 160, 24);
        country_ = control(L"EDIT", amp::wide(settings_.country).c_str(), WS_TABSTOP | ES_UPPERCASE, 190, 80, 55, 26);
        SendMessageW(country_, EM_SETLIMITTEXT, 2, 0);
        artwork_ = control(L"BUTTON", L"Use public album artwork", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 120, 445, 24, id_artwork);
        motion_ = control(L"BUTTON", L"Use existing hosted animated covers", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 150, 445, 24, id_motion);
        startup_ = control(L"BUTTON", L"Start with Windows", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 180, 445, 24);
        SendMessageW(artwork_, BM_SETCHECK, settings_.artwork ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(motion_, BM_SETCHECK, settings_.motion_artwork ? BST_CHECKED : BST_UNCHECKED, 0);
        startup_initial_ = !demo_ && startup_enabled();
        SendMessageW(startup_, BM_SETCHECK, startup_initial_ ? BST_CHECKED : BST_UNCHECKED, 0);
        control(L"STATIC", L"Artwork sends track tags to Apple. New animations are\nnot downloaded by this version.", 0, 20, 214, 450, 46);
        control(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 270, 90, 28, id_save);
        control(L"STATIC", L"Closing this window keeps sharing. Exit from the tray icon.", 0, 20, 309, 450, 24);
        status_control_ = control(L"STATIC", L"Starting…", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 20, 341, 450, 24);
        track_control_ = control(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 20, 370, 450, 24);
        if (demo_) for (HWND item : {client_, country_, artwork_, motion_, startup_}) EnableWindow(item, FALSE);
    }
    void add_tray() {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window_; data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP; data.hIcon = icon_; data.uCallbackMessage = tray_message;
        wcscpy_s(data.szTip, L"Apple Music Presence");
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (!tray_added_) { show(); return; }
        data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    void remove_tray() {
        if (!tray_added_) return;
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window_; data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data); tray_added_ = false;
    }
    void show() { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetTimer(window_, 1, 1000, nullptr); refresh(); }
    void refresh() {
        amp::Status status; { std::lock_guard lock(mutex_); status = status_; }
        std::wstring message = amp::wide(status.message);
        SetWindowTextW(status_control_, message.c_str());
        std::wstring track = status.snapshot.track ? amp::wide(status.snapshot.track->title + " — " + status.snapshot.track->artist) : L"";
        SetWindowTextW(track_control_, track.c_str());
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window_; data.uID = 1; data.uFlags = NIF_TIP | NIF_SHOWTIP;
        auto tip = L"Apple Music Presence\n" + message; tip.resize(std::min<size_t>(tip.size(), 127));
        wcscpy_s(data.szTip, tip.c_str()); if (tray_added_) Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, id_settings, L"Settings");
        AppendMenuW(menu, MF_STRING, id_pause, paused_ ? L"Resume sharing" : L"Pause sharing");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, id_exit, L"Exit");
        POINT location{}; GetCursorPos(&location); SetForegroundWindow(window_);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, location.x, location.y, 0, window_, nullptr);
        DestroyMenu(menu); if (command) PostMessageW(window_, WM_COMMAND, command, 0);
        PostMessageW(window_, WM_NULL, 0, 0);
    }
    void save() {
        if (demo_) { ShowWindow(window_, SW_HIDE); KillTimer(window_, 1); return; }
        auto changed = settings_;
        changed.client_id = amp::utf8(control_text(client_)); changed.country = amp::utf8(control_text(country_));
        std::transform(changed.country.begin(), changed.country.end(), changed.country.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        changed.artwork = SendMessageW(artwork_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        changed.motion_artwork = SendMessageW(motion_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (changed.motion_artwork) changed.artwork = true;
        changed.start_with_windows = SendMessageW(startup_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        try {
            amp::validate_settings(changed);
            bool change_startup = changed.start_with_windows != startup_initial_;
            auto previous_startup = change_startup ? startup_value() : std::optional<StartupValue>{};
            if (change_startup) set_startup(changed.start_with_windows);
            try { amp::save_settings(changed, directory_); }
            catch (...) { if (change_startup) restore_startup(previous_startup); throw; }
            startup_initial_ = changed.start_with_windows;
            settings_ = changed; service_->configure(settings_); ShowWindow(window_, SW_HIDE); KillTimer(window_, 1);
        } catch (const std::exception& error) { MessageBoxW(window_, amp::wide(error.what()).c_str(), L"Check setup", MB_OK | MB_ICONERROR); }
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ && message == taskbar_created_) { tray_added_ = false; add_tray(); return 0; }
        if (message == status_message) { refresh(); return 0; }
        if (message == tray_message) {
            auto event = LOWORD(lparam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) show();
            else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) menu();
            return 0;
        }
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case id_settings: show(); break;
            case id_pause: paused_ = !paused_; service_->pause(paused_); break;
            case id_save: save(); break;
            case id_artwork: if (SendMessageW(artwork_, BM_GETCHECK, 0, 0) != BST_CHECKED) SendMessageW(motion_, BM_SETCHECK, BST_UNCHECKED, 0); break;
            case id_motion: if (SendMessageW(motion_, BM_GETCHECK, 0, 0) == BST_CHECKED) SendMessageW(artwork_, BM_SETCHECK, BST_CHECKED, 0); break;
            case id_exit: remove_tray(); ShowWindow(window_, SW_HIDE); service_.reset(); DestroyWindow(window_); break;
            }
            return 0;
        case WM_CLOSE: if (tray_added_) { ShowWindow(window_, SW_HIDE); KillTimer(window_, 1); } else PostMessageW(window_, WM_COMMAND, id_exit, 0); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOW));
            SetTextColor(reinterpret_cast<HDC>(wparam), GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(lparam)) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_ENDSESSION: if (wparam) { remove_tray(); service_.reset(); DestroyWindow(window_); } return 0;
        case WM_TIMER: if (wparam == 2) PostMessageW(window_, WM_COMMAND, id_exit, 0); else if (IsWindowVisible(window_)) refresh(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
    amp::Settings settings_;
    std::filesystem::path directory_;
    bool demo_{}, paused_{}, tray_added_{}, startup_initial_{};
    double seconds_{};
    HWND window_{}, client_{}, country_{}, artwork_{}, motion_{}, startup_{}, status_control_{}, track_control_{};
    UINT taskbar_created_{};
    HICON icon_{}; HFONT font_{};
    std::mutex mutex_;
    amp::Status status_;
    std::unique_ptr<amp::Service> service_;
};
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleton{};
    bool display_errors = true;
    try {
        int count{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv) throw std::runtime_error("Could not read launch options");
        for (int i = 1; i < count; ++i) if (std::wstring_view(argv[i]) == L"--headless" || std::wstring_view(argv[i]) == L"--diagnose") display_errors = false;
        bool demo{}, headless{}, diagnose{}; double seconds{};
        for (int i = 1; i < count; ++i) {
            std::wstring argument = argv[i];
            if (argument == L"--demo") demo = true;
            else if (argument == L"--headless") headless = true;
            else if (argument == L"--diagnose") diagnose = true;
            else if (argument == L"--seconds" && i + 1 < count) {
                std::wstring number = argv[++i]; size_t end{}; seconds = std::stod(number, &end);
                if (end != number.size() || !std::isfinite(seconds) || seconds <= 0 || seconds > 86400)
                    throw std::runtime_error("--seconds must be between 0 and 86400");
            } else { LocalFree(argv); throw std::runtime_error("Unknown launch option"); }
        }
        LocalFree(argv);
        if (headless && seconds == 0) throw std::runtime_error("--headless requires --seconds for a bounded diagnostic run");
        auto directory = amp::data_directory(); auto settings = demo ? amp::Settings{} : amp::load_settings(directory);
        if (diagnose) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            { amp::MediaBackend media(wake, settings.source_id); auto snapshot = media.read();
              nlohmann::json result{{"has_track", snapshot.track.has_value()}, {"state", static_cast<int>(snapshot.state)},
                  {"has_position", snapshot.position.has_value()}, {"has_duration", snapshot.duration.has_value()}};
              if (snapshot.track) result["metadata_complete"] = !snapshot.track->title.empty() && !snapshot.track->artist.empty() && !snapshot.track->album.empty();
              std::cout << result.dump() << std::endl; }
            CloseHandle(wake); winrt::uninit_apartment(); return 0;
        }
        if (!demo) {
            singleton = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicPresence.Native");
            if (!singleton) throw std::runtime_error("Could not start the app");
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                if (auto window = FindWindowW(window_class, nullptr)) PostMessageW(window, WM_COMMAND, id_settings, 0);
                CloseHandle(singleton); return 0;
            }
        }
        int result{};
        if (headless) {
            if (!demo) amp::validate_settings(settings);
            std::string previous;
            std::mutex output;
            amp::Service service(settings, directory, [&](const amp::Status& status) {
                std::lock_guard lock(output); if (status.message != previous) { std::cout << status.message << std::endl; previous = status.message; }
            }, demo);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
        } else { TrayApp app(settings, directory, demo, seconds); result = app.run(instance); }
        if (singleton) CloseHandle(singleton); return result;
    } catch (const std::exception& error) {
        if (singleton) CloseHandle(singleton);
        std::cerr << "Apple Music Presence: " << error.what() << std::endl;
        if (display_errors) MessageBoxW(nullptr, amp::wide(error.what()).c_str(), L"Apple Music Presence", MB_OK | MB_ICONERROR);
        return 1;
    }
}
