#pragma once

#include <windows.h>
#include <appmodel.h>
#include <d3d11.h>
#undef small  // rpcndr.h's char, on the way in with d3d11.h: a name the pad's layout uses

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Sensors.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Display.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Popups.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace revc {

// The first of the phone's languages (Settings > Time & language > Language), as "ru-RU"
inline std::wstring PhoneLanguage() {
    try {
        auto languages = winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();
        if (languages.Size() > 0) return std::wstring(languages.GetAt(0));
    } catch (...) {
    }
    return {};
}

// The phone's language decides between the two the shell speaks: Russian on a phone set to
// Russian, English on every other. REVC_LANG=en (or ru), a setting for a test run (App.cpp),
// shows the one the phone is not set to.
inline bool Russian() {
    static const bool ru = [] {
        const wchar_t* forced = _wgetenv(L"REVC_LANG");
        std::wstring tag = forced ? std::wstring(forced) : PhoneLanguage();
        // "ru", "ru-RU", "ru-BY": not another language whose tag begins with the same two letters
        return tag.size() >= 2 && towlower(tag[0]) == L'r' && towlower(tag[1]) == L'u' &&
               (tag.size() == 2 || tag[2] == L'-' || tag[2] == L'_');
    }();
    return ru;
}

inline const wchar_t* T(const wchar_t* en, const wchar_t* ru) { return Russian() ? ru : en; }

// A message with one button. The button is the shell's own and not the one a dialog gets by
// itself, so that it is in the language of the text over it whatever the system would put there.
inline winrt::Windows::UI::Popups::MessageDialog Dialog(winrt::hstring const& text, winrt::hstring const& title = {}) {
    winrt::Windows::UI::Popups::MessageDialog dialog(text, title);
    try {
        dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(T(L"Close", L"Закрыть")));
        dialog.DefaultCommandIndex(0);
        dialog.CancelCommandIndex(0);  // the Back key
    } catch (...) {
    }
    return dialog;
}

inline std::string Utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring Wide(const char* text) {
    if (!text || !*text) return {};
    // UTF-8 if it is valid UTF-8; the game's own messages and paths are in the ANSI code page
    UINT page = CP_UTF8;
    int n = ::MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (n <= 0) {
        page = CP_ACP;
        n = ::MultiByteToWideChar(page, 0, text, -1, nullptr, 0);
    }
    if (n <= 1) return {};
    std::wstring w(n - 1, L'\0');
    ::MultiByteToWideChar(page, 0, text, -1, w.data(), n);
    return w;
}

inline winrt::Windows::UI::Xaml::Media::SolidColorBrush Brush(uint32_t argb) {
    return winrt::Windows::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(
        static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
        static_cast<uint8_t>(argb)));
}

inline bool FileExists(std::wstring const& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    return ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
}

}  // namespace revc
