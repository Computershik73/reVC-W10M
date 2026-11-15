// A real gamepad (Bluetooth or USB, through Windows.Gaming.Input), read by the game thread
// once a frame. While one is connected it is the game's pad and the on-screen one is hidden.
#pragma once

#include "host_api.h"

namespace revc {

class Gamepad {
public:
    using Sink = std::function<void(bool connected)>;

    // The sink runs on whatever thread Windows reports the change on
    explicit Gamepad(Sink changed) : changed_(std::move(changed)) {
        using winrt::Windows::Gaming::Input::Gamepad;
        try {
            Gamepad::GamepadAdded([this](auto const&, Gamepad const&) { Refresh(); });
            Gamepad::GamepadRemoved([this](auto const&, Gamepad const&) { Refresh(); });
            Refresh();
        } catch (...) {
            // no gamepad support on this build: the on-screen pad it is
        }
    }

    bool Connected() {
        std::lock_guard<std::mutex> lock(lock_);
        return pad_ != nullptr;
    }

    // False when there is none; the pad is then left alone
    bool Read(HostPad* out) {
        winrt::Windows::Gaming::Input::Gamepad pad{nullptr};
        {
            std::lock_guard<std::mutex> lock(lock_);
            pad = pad_;
        }
        if (!pad) return false;
        using winrt::Windows::Gaming::Input::GamepadButtons;
        winrt::Windows::Gaming::Input::GamepadReading reading{};
        try {
            reading = pad.GetCurrentReading();
        } catch (...) {
            return false;
        }
        auto held = [&](GamepadButtons button) -> uint8_t {
            return (static_cast<uint32_t>(reading.Buttons) & static_cast<uint32_t>(button)) ? 1 : 0;
        };
        memset(out, 0, sizeof(*out));
        out->buttons[HOST_PAD_A] = held(GamepadButtons::A);
        out->buttons[HOST_PAD_B] = held(GamepadButtons::B);
        out->buttons[HOST_PAD_X] = held(GamepadButtons::X);
        out->buttons[HOST_PAD_Y] = held(GamepadButtons::Y);
        out->buttons[HOST_PAD_LB] = held(GamepadButtons::LeftShoulder);
        out->buttons[HOST_PAD_RB] = held(GamepadButtons::RightShoulder);
        out->buttons[HOST_PAD_BACK] = held(GamepadButtons::View);
        out->buttons[HOST_PAD_START] = held(GamepadButtons::Menu);
        out->buttons[HOST_PAD_LTHUMB] = held(GamepadButtons::LeftThumbstick);
        out->buttons[HOST_PAD_RTHUMB] = held(GamepadButtons::RightThumbstick);
        out->buttons[HOST_PAD_UP] = held(GamepadButtons::DPadUp);
        out->buttons[HOST_PAD_RIGHT] = held(GamepadButtons::DPadRight);
        out->buttons[HOST_PAD_DOWN] = held(GamepadButtons::DPadDown);
        out->buttons[HOST_PAD_LEFT] = held(GamepadButtons::DPadLeft);
        // Windows has up as positive, GLFW (and the game) down
        out->axes[HOST_AXIS_LX] = static_cast<float>(reading.LeftThumbstickX);
        out->axes[HOST_AXIS_LY] = static_cast<float>(-reading.LeftThumbstickY);
        out->axes[HOST_AXIS_RX] = static_cast<float>(reading.RightThumbstickX);
        out->axes[HOST_AXIS_RY] = static_cast<float>(-reading.RightThumbstickY);
        // 0..1 here, -1 (released) .. 1 for the game
        out->axes[HOST_AXIS_LT] = static_cast<float>(reading.LeftTrigger * 2.0 - 1.0);
        out->axes[HOST_AXIS_RT] = static_cast<float>(reading.RightTrigger * 2.0 - 1.0);
        return true;
    }

private:
    void Refresh() {
        winrt::Windows::Gaming::Input::Gamepad first{nullptr};
        try {
            auto pads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
            if (pads.Size() > 0) first = pads.GetAt(0);
        } catch (...) {
        }
        bool was, is = first != nullptr;
        {
            std::lock_guard<std::mutex> lock(lock_);
            was = pad_ != nullptr;
            pad_ = first;
        }
        if (was != is && changed_) changed_(is);
    }

    std::mutex lock_;
    winrt::Windows::Gaming::Input::Gamepad pad_{nullptr};
    Sink changed_;
};

}  // namespace revc
