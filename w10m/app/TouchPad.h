// The on-screen pad: XAML elements over the SwapChainPanel, laid out for what the player is
// doing (host_api.h: a menu, on foot, in a vehicle, a cutscene). The game sees it as a gamepad
// with the PlayStation 2 layout the game was made for:
//
//   ✕ sprint / accelerate     □ jump / brake, reverse     ○ fire     △ enter, leave a vehicle
//   L1 answer the phone / radio station     R1 aim, lock on / handbrake
//   L2, R2 previous and next weapon / look left and right (both: look behind)
//   L3 crouch / horn     R3 look behind / start or stop a vehicle's missions (taxi, ambulance...)
//   CAM change the camera     ≡ pause
//
// The buttons wear the names of the controller type set in the game (PlayStation's above,
// Xbox's A B X Y LB..., the Switch's), because the game's prompts are drawn with that type's
// pictures: "press L1" must be a button that says L1. In a vehicle two more, ♪◀ and ♪▶, tune
// the radio back and on: nothing on the pad says that L1 is the radio, and no button goes back.
//
// The sticks have no fixed place: a finger put down on the left half of the screen (off the
// buttons) is the left stick, centred where it landed, and on the right half the right one,
// which turns the camera. XAML gives every element its own pointer stream, so sticks and
// buttons work together.
//
// In a menu a touch is the mouse (the game's menus take one), next to a d-pad, ✕ and △
// (back). Everything here runs on the UI thread; the game thread reads TouchState.
#pragma once

#include "host_api.h"

namespace revc {

struct TouchState {
    std::mutex lock;
    HostPad pad{};
    TouchState() { pad.axes[HOST_AXIS_LT] = pad.axes[HOST_AXIS_RT] = -1.0f; }
};

class TouchPad {
public:
    // type: HOST_EV_POINTER_*; x, y in the stage's DIPs
    using MouseSink = std::function<void(int32_t type, float x, float y)>;
    using Request = std::function<void(int32_t request)>;  // HOST_REQUEST_*: asked of the game
    using Action = std::function<void()>;

    TouchPad(winrt::Windows::UI::Xaml::Controls::Grid const& stage, TouchState& state, MouseSink mouse, Request request,
             Action resolution)
        : state_(state), mouse_(std::move(mouse)), request_(std::move(request)), resolution_(std::move(resolution)) {
        using namespace winrt::Windows::UI::Xaml;
        root_ = Controls::Canvas();  // no background: touches off the buttons reach the panel under it
        stage.Children().Append(root_);
        for (int id : std::initializer_list<int>{HOST_PAD_A, HOST_PAD_B, HOST_PAD_X, HOST_PAD_Y, HOST_PAD_LB,
                                                 HOST_PAD_RB, HOST_PAD_BACK, HOST_PAD_START, HOST_PAD_LTHUMB,
                                                 HOST_PAD_RTHUMB, HOST_PAD_UP, HOST_PAD_RIGHT, HOST_PAD_DOWN,
                                                 HOST_PAD_LEFT, kLeftTrigger, kRightTrigger, kSkip, kResolution,
                                                 kRadioPrevious, kRadioNext, kStyle})
            AddButton(id);
        for (Stick* stick : {&left_, &right_}) {
            stick->ring = Circle(Brush(0x14FFFFFF), Brush(0x66FFFFFF));
            stick->knob = Circle(Brush(0x50FFFFFF), Brush(0x99FFFFFF));
            root_.Children().Append(stick->ring);
            root_.Children().Append(stick->knob);
        }
    }

    // The stage's size in DIPs: every position is worked out from it
    void Layout(double width, double height) {
        // only when it changed: arranging lets go of everything that is held
        if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
        width_ = width;
        height_ = height;
        Arrange();
    }

    void Context(int32_t context) {
        if (context == context_) return;
        context_ = context;
        Arrange();
    }

    int32_t Context() const { return context_; }

    // HOST_PAD_STYLE_*: whose names the buttons wear
    void Style(int32_t style) {
        if (style == style_) return;
        style_ = style;
        Arrange();
    }

    // Off while a real gamepad is connected: it has the buttons, the screen only the picture.
    // A touch in a menu stays a mouse click either way.
    void Enabled(bool on) {
        if (on == enabled_) return;
        enabled_ = on;
        Arrange();
    }

    void ResolutionLabel(std::wstring const& text) {
        resolutionLabel_ = text;
        if (Button* b = Find(kResolution)) b->text.Text(winrt::hstring(text));
    }

    // Pointers that came down on the panel itself, in the stage's DIPs
    void PanelPressed(int32_t pointer, winrt::Windows::Foundation::Point p) {
        if (context_ == HOST_CONTEXT_MENU) {
            if (menuPointer_ < 0) {
                menuPointer_ = pointer;
                menuAt_ = p;
                mouse_(HOST_EV_POINTER_DOWN, p.X, p.Y);
            }
            return;
        }
        if (!enabled_ || (context_ != HOST_CONTEXT_ON_FOOT && context_ != HOST_CONTEXT_VEHICLE)) return;
        Stick& stick = p.X < width_ / 2 ? left_ : right_;
        if (stick.owner >= 0) return;
        stick.owner = pointer;
        stick.origin = p;
        Show(stick, true);
        Steer(stick, p);
    }

    void PanelMoved(int32_t pointer, winrt::Windows::Foundation::Point p) {
        if (pointer == menuPointer_) {
            menuAt_ = p;
            mouse_(HOST_EV_POINTER_MOVE, p.X, p.Y);
        } else if (pointer == left_.owner) Steer(left_, p);
        else if (pointer == right_.owner) Steer(right_, p);
    }

    void PanelReleased(int32_t pointer, winrt::Windows::Foundation::Point p) {
        if (pointer == menuPointer_) {
            menuPointer_ = -1;
            mouse_(HOST_EV_POINTER_UP, p.X, p.Y);
        } else if (pointer == left_.owner) {
            Release(left_);
        } else if (pointer == right_.owner) {
            Release(right_);
        }
    }

private:
    // Ids past the gamepad's buttons: the triggers (axes to the game), and those that are not
    // buttons of a gamepad at all
    enum { kLeftTrigger = 100, kRightTrigger, kSkip, kResolution, kRadioPrevious, kRadioNext, kStyle };

    // A button's name on the controller whose names are worn. The Switch's are the game's own
    // (ControllerConfig.cpp), which go by what a button does: its Y is where the triangle is.
    const wchar_t* Name(int id) const {
        static const wchar_t* const names[3][10] = {
            {L"✕", L"○", L"□", L"△", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3"},
            {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"LS", L"RS"},
            {L"B", L"A", L"X", L"Y", L"L", L"R", L"ZL", L"ZR", L"LS", L"RS"},
        };
        int column = id == HOST_PAD_A        ? 0
                     : id == HOST_PAD_B      ? 1
                     : id == HOST_PAD_X      ? 2
                     : id == HOST_PAD_Y      ? 3
                     : id == HOST_PAD_LB     ? 4
                     : id == HOST_PAD_RB     ? 5
                     : id == kLeftTrigger    ? 6
                     : id == kRightTrigger   ? 7
                     : id == HOST_PAD_LTHUMB ? 8
                     : id == HOST_PAD_RTHUMB ? 9
                                             : -1;
        int row = style_ == HOST_PAD_STYLE_XBOX ? 1 : style_ == HOST_PAD_STYLE_NINTENDO ? 2 : 0;
        return column < 0 ? L"" : names[row][column];
    }

    struct Button {
        int id = 0;
        winrt::Windows::UI::Xaml::Controls::Grid grid{nullptr};
        winrt::Windows::UI::Xaml::Shapes::Rectangle shape{nullptr};
        winrt::Windows::UI::Xaml::Controls::TextBlock text{nullptr};
        int32_t owner = -1;  // the pointer holding it down
    };

    struct Stick {
        int32_t owner = -1;
        winrt::Windows::Foundation::Point origin{};
        winrt::Windows::UI::Xaml::Shapes::Ellipse ring{nullptr}, knob{nullptr};
        int axisX = 0, axisY = 0;
    };

    static winrt::Windows::UI::Xaml::Shapes::Ellipse Circle(winrt::Windows::UI::Xaml::Media::Brush const& fill,
                                                            winrt::Windows::UI::Xaml::Media::Brush const& stroke) {
        winrt::Windows::UI::Xaml::Shapes::Ellipse e;
        e.Fill(fill);
        e.Stroke(stroke);
        e.StrokeThickness(2);
        e.IsHitTestVisible(false);
        e.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        return e;
    }

    Button* Find(int id) {
        for (auto& b : buttons_)
            if (b->id == id) return b.get();
        return nullptr;
    }

    void AddButton(int id) {
        using namespace winrt::Windows::UI::Xaml;
        auto b = std::make_unique<Button>();
        b->id = id;
        b->grid = Controls::Grid();
        b->shape = Shapes::Rectangle();  // a circle or a pill: the corners are set with the size
        b->shape.Fill(Brush(kIdle));
        b->shape.Stroke(Brush(0x78FFFFFF));
        b->shape.StrokeThickness(2);
        b->grid.Children().Append(b->shape);
        b->text = Controls::TextBlock();
        b->text.Foreground(Brush(0xC8FFFFFF));
        b->text.HorizontalAlignment(HorizontalAlignment::Center);
        b->text.VerticalAlignment(VerticalAlignment::Center);
        b->text.TextAlignment(TextAlignment::Center);
        b->text.IsHitTestVisible(false);
        b->grid.Children().Append(b->text);
        b->grid.Visibility(Visibility::Collapsed);
        root_.Children().Append(b->grid);

        Button* button = b.get();
        using winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs;
        button->grid.PointerPressed([this, button](auto const&, PointerRoutedEventArgs const& a) {
            if (button->owner < 0) {
                button->owner = static_cast<int32_t>(a.Pointer().PointerId());
                Press(*button, true);
            }
            button->grid.CapturePointer(a.Pointer());
            a.Handled(true);
        });
        auto release = [this, button](auto const&, PointerRoutedEventArgs const& a) {
            if (button->owner == static_cast<int32_t>(a.Pointer().PointerId())) {
                button->owner = -1;
                Press(*button, false);
            }
            a.Handled(true);
        };
        button->grid.PointerReleased(release);
        button->grid.PointerCanceled(release);
        button->grid.PointerCaptureLost(release);
        buttons_.push_back(std::move(b));
    }

    void Press(Button& b, bool down) {
        b.shape.Fill(Brush(down ? kHeld : kIdle));
        if (b.id == kSkip) {
            if (down) request_(HOST_REQUEST_SKIP_CUTSCENE);
        } else if (b.id == kRadioPrevious) {
            if (down) request_(HOST_REQUEST_RADIO_PREVIOUS);
        } else if (b.id == kRadioNext) {
            if (down) request_(HOST_REQUEST_RADIO_NEXT);
        } else if (b.id == kStyle) {
            if (down) request_(HOST_REQUEST_NEXT_PAD_STYLE);
        } else if (b.id == kResolution) {
            if (down) resolution_();
        } else {
            std::lock_guard<std::mutex> lock(state_.lock);
            if (b.id == kLeftTrigger) state_.pad.axes[HOST_AXIS_LT] = down ? 1.0f : -1.0f;
            else if (b.id == kRightTrigger) state_.pad.axes[HOST_AXIS_RT] = down ? 1.0f : -1.0f;
            else state_.pad.buttons[b.id] = down ? 1 : 0;
        }
    }

    // Deflection of a stick from where the finger landed. The game's own dead zone is 0.3 of
    // the range (skel/glfw/glfw.cpp): past a small dead zone here the output starts above it,
    // so that a short move of the thumb already walks, or turns the camera slowly.
    void Steer(Stick& stick, winrt::Windows::Foundation::Point p) {
        double reach = stickRadius_ > 0 ? stickRadius_ : 1;
        double dx = (p.X - stick.origin.X) / reach, dy = (p.Y - stick.origin.Y) / reach;
        double length = std::sqrt(dx * dx + dy * dy);
        if (length > 1) {
            dx /= length;
            dy /= length;
            length = 1;
        }
        Place(stick.knob, stick.origin.X + dx * reach, stick.origin.Y + dy * reach, reach * 0.45);
        double x = 0, y = 0;
        if (length > 0.08) {
            double out = 0.32 + 0.68 * (length - 0.08) / 0.92;
            x = dx / length * out;
            y = dy / length * out;
        }
        std::lock_guard<std::mutex> lock(state_.lock);
        state_.pad.axes[stick.axisX] = static_cast<float>(x);
        state_.pad.axes[stick.axisY] = static_cast<float>(y);
    }

    void Show(Stick& stick, bool on) {
        auto visibility = on ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed;
        if (on) Place(stick.ring, stick.origin.X, stick.origin.Y, stickRadius_);
        stick.ring.Visibility(visibility);
        stick.knob.Visibility(visibility);
    }

    void Release(Stick& stick) {
        stick.owner = -1;
        Show(stick, false);
        std::lock_guard<std::mutex> lock(state_.lock);
        state_.pad.axes[stick.axisX] = state_.pad.axes[stick.axisY] = 0;
    }

    static void Place(winrt::Windows::UI::Xaml::FrameworkElement const& element, double cx, double cy, double radius) {
        element.Width(radius * 2);
        element.Height(radius * 2);
        winrt::Windows::UI::Xaml::Controls::Canvas::SetLeft(element, cx - radius);
        winrt::Windows::UI::Xaml::Controls::Canvas::SetTop(element, cy - radius);
    }

    // A button of half-width rx and half-height ry around (cx, cy): a circle when they are equal
    void Put(int id, double cx, double cy, double rx, double ry, const wchar_t* label) {
        Button* b = Find(id);
        if (!b) return;
        b->grid.Width(rx * 2);
        b->grid.Height(ry * 2);
        b->shape.RadiusX((std::min)(rx, ry));
        b->shape.RadiusY((std::min)(rx, ry));
        winrt::Windows::UI::Xaml::Controls::Canvas::SetLeft(b->grid, cx - rx);
        winrt::Windows::UI::Xaml::Controls::Canvas::SetTop(b->grid, cy - ry);
        size_t length = wcslen(label);
        b->text.Text(label);
        b->text.FontSize((std::min)(rx, ry) * (length <= 1 ? 0.95 : length <= 2 ? 0.72 : 0.5));
        b->grid.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
    }

    void Put(int id, double cx, double cy, double radius, const wchar_t* label) { Put(id, cx, cy, radius, radius, label); }

    // A button of the gamepad, under the name it has on the controller in use
    void Put(int id, double cx, double cy, double radius) { Put(id, cx, cy, radius, radius, Name(id)); }

    // Nothing stays down across a change of what is on the screen
    void ReleaseAll() {
        for (auto& b : buttons_) {
            if (b->owner >= 0) {
                b->owner = -1;
                Press(*b, false);
            }
        }
        if (left_.owner >= 0) Release(left_);
        if (right_.owner >= 0) Release(right_);
        if (menuPointer_ >= 0) {
            menuPointer_ = -1;
            mouse_(HOST_EV_POINTER_UP, menuAt_.X, menuAt_.Y);  // the button comes up where it was
        }
        std::lock_guard<std::mutex> lock(state_.lock);
        memset(state_.pad.buttons, 0, sizeof(state_.pad.buttons));
        for (int axis : {HOST_AXIS_LX, HOST_AXIS_LY, HOST_AXIS_RX, HOST_AXIS_RY}) state_.pad.axes[axis] = 0;
        state_.pad.axes[HOST_AXIS_LT] = state_.pad.axes[HOST_AXIS_RT] = -1.0f;
    }

    void Arrange() {
        ReleaseAll();
        for (auto& b : buttons_) b->grid.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        left_.axisX = HOST_AXIS_LX;
        left_.axisY = HOST_AXIS_LY;
        right_.axisX = HOST_AXIS_RX;
        right_.axisY = HOST_AXIS_RY;
        if (width_ <= 0 || height_ <= 0) return;

        const double W = width_, H = height_;
        const double unit = (std::min)(W, H);
        const double margin = unit * 0.045;
        const double big = unit * 0.066;    // the face buttons
        const double small = unit * 0.052;  // shoulders, sticks' clicks, the top row
        stickRadius_ = unit * 0.16;

        if (context_ == HOST_CONTEXT_MENU) {
            // The resolution button is the shell's own setting, shown where the game's settings are;
            // the one beside it is the game's controller type, PlayStation or Xbox, at hand
            Put(kResolution, W - margin - big * 1.5, margin + small, big * 1.5, small, resolutionLabel_.c_str());
            Put(kStyle, W - margin - big * 4.8, margin + small, big * 1.5, small,
                style_ == HOST_PAD_STYLE_XBOX ? L"XBOX" : style_ == HOST_PAD_STYLE_NINTENDO ? L"SWITCH" : L"PS");
            if (!enabled_) return;
            double cx = margin + big * 3.1, cy = H - margin - big * 3.1;
            Put(HOST_PAD_UP, cx, cy - big * 2.1, big, L"▲");
            Put(HOST_PAD_DOWN, cx, cy + big * 2.1, big, L"▼");
            Put(HOST_PAD_LEFT, cx - big * 2.1, cy, big, L"◀");
            Put(HOST_PAD_RIGHT, cx + big * 2.1, cy, big, L"▶");
            // back is Triangle in this game (TRIANGLE_BACK_BUTTON), not Circle
            Put(HOST_PAD_Y, W - margin - big * 1.3, H - margin - big * 4.2, big * 1.3);
            Put(HOST_PAD_A, W - margin - big * 1.3, H - margin - big * 1.3, big * 1.3);
            return;
        }
        if (!enabled_) return;
        if (context_ == HOST_CONTEXT_CUTSCENE) {
            Put(kSkip, W - margin - big * 2.6, H - margin - big, big * 2.6, big, T(L"SKIP", L"ДАЛЬШЕ"));
            return;
        }
        if (context_ != HOST_CONTEXT_ON_FOOT && context_ != HOST_CONTEXT_VEHICLE) return;  // loading

        // The top row, clear of the radar (top left, about a fifth of the width)
        Put(HOST_PAD_BACK, W * 0.27, margin + small, small * 1.5, small, L"CAM");
        Put(HOST_PAD_START, W - margin - small, margin + small, small, L"≡");

        // Shoulders in a row on the left, above where the thumb rests, the stick's click over them
        double rowY = H * 0.47, rowX = margin + small;
        Put(kLeftTrigger, rowX, rowY, small);
        Put(HOST_PAD_LB, rowX + small * 2.5, rowY, small);
        Put(HOST_PAD_RB, rowX + small * 5.0, rowY, small);
        Put(kRightTrigger, rowX + small * 7.5, rowY, small);
        Put(HOST_PAD_LTHUMB, rowX + small * 3.75, rowY - small * 2.4, small);

        if (context_ == HOST_CONTEXT_ON_FOOT) {
            // the four in a diamond, as on the pad
            double cx = W - margin - big * 3.1, cy = H - margin - big * 3.1;
            Put(HOST_PAD_Y, cx, cy - big * 2.1, big);
            Put(HOST_PAD_A, cx, cy + big * 2.1, big);
            Put(HOST_PAD_X, cx - big * 2.1, cy, big);
            Put(HOST_PAD_B, cx + big * 2.1, cy, big);
            Put(HOST_PAD_RTHUMB, cx, cy - big * 3.1 - small * 1.4, small);
        } else {
            // two pedals under the right thumb, the rest above them
            double gas = big * 1.55, brake = big * 1.3;
            double gasX = W - margin - gas, gasY = H - margin - gas;
            double brakeX = gasX - gas - brake - big * 0.5, brakeY = H - margin - brake;
            Put(HOST_PAD_A, gasX, gasY, gas);
            Put(HOST_PAD_X, brakeX, brakeY, brake);
            Put(HOST_PAD_B, gasX, gasY - gas - big * 1.5, big);
            Put(HOST_PAD_Y, brakeX, brakeY - brake - big * 1.5, big);
            Put(HOST_PAD_RTHUMB, gasX, gasY - gas - big * 3.0 - small * 1.4, small);
            // the radio, either side of the stick's click over the shoulders
            Put(kRadioPrevious, rowX, rowY - small * 2.4, small, L"♪◀");
            Put(kRadioNext, rowX + small * 7.5, rowY - small * 2.4, small, L"♪▶");
        }
    }

    static constexpr uint32_t kIdle = 0x30FFFFFF;
    static constexpr uint32_t kHeld = 0x80FFFFFF;

    TouchState& state_;
    MouseSink mouse_;
    Request request_;
    Action resolution_;
    winrt::Windows::UI::Xaml::Controls::Canvas root_{nullptr};
    std::vector<std::unique_ptr<Button>> buttons_;
    Stick left_, right_;
    std::wstring resolutionLabel_;
    double width_ = 0, height_ = 0, stickRadius_ = 0;
    int32_t context_ = HOST_CONTEXT_LOADING;
    int32_t style_ = HOST_PAD_STYLE_PLAYSTATION;
    int32_t menuPointer_ = -1;
    winrt::Windows::Foundation::Point menuAt_{};
    bool enabled_ = true;
};

}  // namespace revc
