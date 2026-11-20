// Windows Phone 8.1's Back key (Windows.Phone.UI.Input.HardwareButtons), for the shell
// (app/App.cpp, REVC_WP81). Windows 10 has SystemNavigationManager.BackRequested instead, and
// the SDK's C++/WinRT headers have no projection of this phone class: its two interfaces and
// the event's delegate, by the ABI, as the Windows Phone 8.1 kit's
// Include\abi\windows.phone.ui.input.h declares them.
#pragma once

#include <eventtoken.h>
#include <inspectable.h>

#include <functional>

namespace revc::wp81 {

struct __declspec(uuid("F6F555FF-64EC-42A2-B93B-2FBC0C36A121")) IBackPressedEventArgs : ::IInspectable {
    virtual HRESULT __stdcall get_Handled(boolean* value) = 0;
    virtual HRESULT __stdcall put_Handled(boolean value) = 0;
};

// EventHandler<BackPressedEventArgs>
struct __declspec(uuid("cb5bc6b5-eee7-57a8-9563-eb0b2b8eb11f")) IBackPressedHandler : ::IUnknown {
    virtual HRESULT __stdcall Invoke(::IInspectable* sender, IBackPressedEventArgs* args) = 0;
};

struct __declspec(uuid("594B8780-DA66-4FD8-A776-7506BD0CBFA7")) IHardwareButtonsStatics : ::IInspectable {
    virtual HRESULT __stdcall add_BackPressed(IBackPressedHandler* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT __stdcall remove_BackPressed(EventRegistrationToken token) = 0;
};

// The handler says whether it took the key (true: the app stays; false: the phone goes back,
// out of the app)
struct BackPressed : winrt::implements<BackPressed, IBackPressedHandler> {
    std::function<bool()> handle;
    explicit BackPressed(std::function<bool()> h) : handle(std::move(h)) {}
    HRESULT __stdcall Invoke(::IInspectable*, IBackPressedEventArgs* args) noexcept override {
        bool taken = false;
        try {
            taken = handle && handle();
        } catch (...) {
        }
        if (args) args->put_Handled(taken ? 1 : 0);
        return S_OK;
    }
};

// False where the class is not there (Windows 10 has it only in its phone contract)
inline bool OnBackPressed(std::function<bool()> handle) {
    try {
        auto statics = winrt::get_activation_factory<IHardwareButtonsStatics>(L"Windows.Phone.UI.Input.HardwareButtons");
        EventRegistrationToken token{};
        auto handler = winrt::make<BackPressed>(std::move(handle));
        return SUCCEEDED(statics->add_BackPressed(handler.as<IBackPressedHandler>().get(), &token));
    } catch (...) {
        return false;
    }
}

}  // namespace revc::wp81
