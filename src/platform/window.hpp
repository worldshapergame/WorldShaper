#pragma once
// Window, input and event pump, on SDL3 (zlib licence).
//
// This is the only file in the engine that knows SDL exists. Everything above it sees a
// plain Window with a size, an input snapshot, and a Vulkan surface.

#include <string>

#include "core/types.hpp"

struct SDL_Window;
union SDL_Event;

namespace ws {

enum class Key : u16 {
    Unknown = 0,
    Escape, Space, Tab, Enter, Backspace,
    W, A, S, D, Q, E, R, F, G, H, C, V, X, Y, Z, O, P, T, U,
    // Tool slots. Contiguous and in order, so slot n is Digit1 + n — anything that relies
    // on that says so at the point of use.
    Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    Left, Right, Up, Down,
    Comma, Period, Slash,
    Shift, Ctrl, Alt,
    // What a text field needs and a camera never did. They arrived with the shell (Stage 15):
    // a value you can type into is a value you can put the caret in the middle of.
    Delete, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F8, F9, F10, F11, F12,
    Count
};

// Held keys that should repeat while down, for things a player spams: rotation steps, copy
// counts. The window reports the raw press; repeat is a policy decision and lives with the
// code that wants it (game/repeat.hpp).
inline constexpr u32 kDigitSlots = 9;

struct InputState {
    bool down[static_cast<usize>(Key::Count)]{};
    bool pressed[static_cast<usize>(Key::Count)]{};   // went down this frame
    bool released[static_cast<usize>(Key::Count)]{};
    // Held long enough that the system said so again. The camera never wanted this — a held W is
    // already a held W — but a held Backspace has to delete more than one character, and the
    // system's own repeat delay and rate are the ones the player set for every other application
    // on their machine. Game/repeat.hpp is still the answer for things a game invents a rate for.
    bool repeated[static_cast<usize>(Key::Count)]{};

    f32 mouse_x = 0.0f;
    f32 mouse_y = 0.0f;
    f32 mouse_dx = 0.0f;
    f32 mouse_dy = 0.0f;
    f32 wheel = 0.0f;
    bool mouse_left = false;
    bool mouse_right = false;
    bool mouse_middle = false;
    // The edges, which a pointer-driven interface needs and a camera does not: a button is
    // answered on the release inside it, not on the press.
    bool mouse_left_pressed = false;
    bool mouse_left_released = false;
    bool mouse_right_pressed = false;
    bool mouse_right_released = false;
    // How many clicks the system counted in this one, so a double-click is the system's idea of
    // one rather than a timer of ours that disagrees with every other window on the desktop.
    u32 click_count = 0;

    // What was typed this frame, as UTF-8. Empty unless a field asked for text input, because
    // that is what turns the platform's composition on.
    std::string typed;

    bool is_down(Key k) const { return down[static_cast<usize>(k)]; }
    bool was_pressed(Key k) const { return pressed[static_cast<usize>(k)]; }
    bool was_released(Key k) const { return released[static_cast<usize>(k)]; }
    // A press or a repeat: what an editing key means. Spelled out here so that no caller has to
    // remember to `||` the two and none of them can forget.
    bool fired(Key k) const { return pressed[static_cast<usize>(k)] || repeated[static_cast<usize>(k)]; }
};

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // `allow_oversize` skips the fit-to-desktop clamp. Off by default so a window can never
    // open bigger than the screen it is on; on when a size was asked for explicitly, because
    // a scripted measurement at 1440p has to be 1440p on a 1366x768 desktop too.
    bool create(const std::string& title, u32 width, u32 height, bool allow_oversize = false);
    void destroy();

    // Pumps the OS event queue. Returns false when the user has asked to quit.
    bool pump();

    // Optional hook so the developer HUD can see events before we consume them.
    using EventHook = void (*)(const SDL_Event&);
    void set_event_hook(EventHook hook) { event_hook_ = hook; }

    void set_relative_mouse(bool enabled);
    bool relative_mouse() const { return relative_mouse_; }

    // Turns the platform's text composition on, which is what makes `InputState::typed` non-empty.
    // Off unless something is being typed into: leaving it on puts an input-method window over the
    // game on a machine set up for a language that needs one, for the whole session, for nothing.
    void set_text_input(bool enabled);
    bool text_input() const { return text_input_; }

    void set_title(const std::string& title);

    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // What the display this window is on actually refreshes at. The default frame-rate target,
    // because rendering faster than the monitor can show is work nobody sees — and on a
    // machine with room to spare that time is better spent on samples than thrown away.
    // Returns 60 when the display will not say, which is the safe guess.
    f32 refresh_hz() const;
    bool minimised() const { return minimised_; }
    bool resized_this_frame() const { return resized_; }

    SDL_Window* handle() const { return window_; }
    const InputState& input() const { return input_; }

    // Instance extensions SDL requires for surface creation. Valid until shutdown.
    static const char* const* required_vulkan_extensions(u32& count);

    // The directory the running executable is in, with a trailing separator. Empty when the
    // platform will not say.
    //
    // This is how a release finds its shaders, and it has to be asked at *run* time. Using
    // the path the build was configured with is an absolute path on the machine that
    // compiled it — which exists there and nowhere else, so every test passed for the
    // developer and every download opened a black window and closed.
    static std::string base_path();

    // Creates the presentation surface. Handles are passed as void* so that this header
    // — the only place SDL is visible — never pulls in Vulkan either.
    bool create_vulkan_surface(void* instance, void** out_surface) const;

private:
    SDL_Window* window_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool minimised_ = false;
    bool resized_ = false;
    bool relative_mouse_ = false;
    bool text_input_ = false;
    bool should_close_ = false;
    InputState input_;
    EventHook event_hook_ = nullptr;
};

}  // namespace ws
