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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace revc {

// The phone's language decides between the two the shell speaks.
inline bool Russian() {
    static const bool ru = [] {
        try {
            auto languages = winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();
            return languages.Size() > 0 && std::wstring(languages.GetAt(0)).rfind(L"ru", 0) == 0;
        } catch (...) {
            return false;
        }
    }();
    return ru;
}

inline const wchar_t* T(const wchar_t* en, const wchar_t* ru) { return Russian() ? ru : en; }

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
