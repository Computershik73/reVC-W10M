/* The seam between the UWP shell (App.cpp: XAML, the UI thread) and the game, which runs on
 * its own thread behind GLFW's API (w10m/glfw) and src/skel/w10m. C only, so that neither side
 * has to see the other's headers. */
#ifndef REVC_W10M_HOST_API_H
#define REVC_W10M_HOST_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOST_EV_KEY_DOWN = 1,   /* code: Windows virtual key */
    HOST_EV_KEY_UP,
    HOST_EV_POINTER_DOWN,   /* x, y: render-target pixels. Sent only while a menu is up, where */
    HOST_EV_POINTER_MOVE,   /* a touch is the mouse; in the game touches drive the pad instead */
    HOST_EV_POINTER_UP,
    HOST_EV_WHEEL,          /* y: notches, up positive */
    HOST_EV_FOCUS,          /* code: 1 gained, 0 lost */
    HOST_EV_VISIBLE,        /* code: 1 shown, 0 hidden (another app, the lock screen) */
    HOST_EV_QUIT,
};

typedef struct {
    int32_t type;
    int32_t code;
    float x, y;
} HostEvent;

/* The pad the game sees as joystick 0: the on-screen controls, or a real gamepad when one is
 * connected (Windows.Gaming.Input). Indices are GLFW's gamepad ones. */
enum {
    HOST_PAD_A, HOST_PAD_B, HOST_PAD_X, HOST_PAD_Y,
    HOST_PAD_LB, HOST_PAD_RB, HOST_PAD_BACK, HOST_PAD_START, HOST_PAD_GUIDE,
    HOST_PAD_LTHUMB, HOST_PAD_RTHUMB,
    HOST_PAD_UP, HOST_PAD_RIGHT, HOST_PAD_DOWN, HOST_PAD_LEFT,
    HOST_PAD_BUTTONS,
};
enum {
    HOST_AXIS_LX, HOST_AXIS_LY, HOST_AXIS_RX, HOST_AXIS_RY, /* -1..1, down and right positive */
    HOST_AXIS_LT, HOST_AXIS_RT,                             /* -1 released .. 1 pressed */
    HOST_PAD_AXES,
};

typedef struct {
    uint8_t buttons[HOST_PAD_BUTTONS];
    float axes[HOST_PAD_AXES];
} HostPad;

/* What the player is doing, so that the shell shows the controls that fit. */
enum {
    HOST_CONTEXT_MENU = 0,
    HOST_CONTEXT_ON_FOOT,
    HOST_CONTEXT_VEHICLE,
    HOST_CONTEXT_CUTSCENE,
    HOST_CONTEXT_LOADING,   /* nothing to press */
};

/* Things the shell asks of the game; the game thread takes them once a frame. */
enum {
    HOST_REQUEST_SKIP_CUTSCENE = 1,
    HOST_REQUEST_PAUSE = 2,     /* the app went to the background: open the pause menu */
    HOST_REQUEST_RADIO_NEXT = 4,        /* the pad's radio buttons: a station on... */
    HOST_REQUEST_RADIO_PREVIOUS = 8,    /* ...or one back, which no gamepad button does */
    HOST_REQUEST_NEXT_PAD_STYLE = 16,   /* the pad's button in the menus: the other controller type */
};

/* Whose names the buttons wear, on the shell's pad as in the game's prompts: the controller
 * type of the game's own menu (Options, Controller setup), which the game tells the shell. */
enum {
    HOST_PAD_STYLE_PLAYSTATION = 0,
    HOST_PAD_STYLE_XBOX,
    HOST_PAD_STYLE_NINTENDO,
};

/* ===[ Implemented by the shell ]=== */

/* The XAML SwapChainPanel as an IInspectable*, which is what ANGLE takes as EGLNativeWindowType. */
void* host_panel(void);
/* Render-target size in pixels: the panel's, times the render scale. False until laid out. */
bool host_panel_size(int32_t* width, int32_t* height);
/* The same before the render scale: the panel in physical pixels. */
bool host_panel_native_size(int32_t* width, int32_t* height);
bool host_poll_event(HostEvent* event);
bool host_has_focus(void);
void host_pad_state(HostPad* pad);
void host_set_context(int32_t context);
void host_set_pad_style(int32_t style);
int32_t host_take_requests(void);
/* "About the port" in the game's Options menu: the shell shows who made the port, over the
 * game, until the player closes it. Returns at once. */
void host_show_about(void);
/* The app's own folder (LocalState: the log is there), for what a test run leaves to be fetched;
 * null where there is none. */
const wchar_t* host_local_folder(void);

/* The Direct3D 11 renderer (w10m/rwd3d11): its swap chain, an IDXGISwapChain1 made for
 * composition, width by height pixels, is to be what the panel shows, stretched over all of
 * it. Again with the same swap chain when its size has changed; with a null one, the panel
 * shows nothing of it any more. Returns once the UI thread has done it; false if it could not. */
bool host_set_swap_chain(void* swapChain, int32_t width, int32_t height);
/* UTF-8, one line; goes to LocalState\revc.log */
void host_log(const char* text);
void host_logf(const char* format, ...);
/* Something the game cannot go on from (a file of the game is missing...): logs it, shows it
 * on the screen and ends the app. Does not return. */
void host_fatal(const char* text);

/* ===[ Implemented by the game ]=== */

/* skel/glfw/glfw.cpp: the game's main loop, on the calling thread. The current directory is
 * the game's folder. */
int reVC_Main(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif

#endif
