#include "platform/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

#include "core/log.hpp"

namespace ws {
namespace {

Key key_from_scancode(SDL_Scancode code) {
    switch (code) {
        case SDL_SCANCODE_ESCAPE:    return Key::Escape;
        case SDL_SCANCODE_SPACE:     return Key::Space;
        case SDL_SCANCODE_TAB:       return Key::Tab;
        case SDL_SCANCODE_RETURN:    return Key::Enter;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_W: return Key::W;
        case SDL_SCANCODE_A: return Key::A;
        case SDL_SCANCODE_S: return Key::S;
        case SDL_SCANCODE_D: return Key::D;
        case SDL_SCANCODE_Q: return Key::Q;
        case SDL_SCANCODE_E: return Key::E;
        case SDL_SCANCODE_R: return Key::R;
        case SDL_SCANCODE_F: return Key::F;
        case SDL_SCANCODE_G: return Key::G;
        case SDL_SCANCODE_H: return Key::H;
        case SDL_SCANCODE_C: return Key::C;
        case SDL_SCANCODE_V: return Key::V;
        case SDL_SCANCODE_X: return Key::X;
        case SDL_SCANCODE_Z: return Key::Z;
        case SDL_SCANCODE_O: return Key::O;
        case SDL_SCANCODE_P: return Key::P;
        case SDL_SCANCODE_T: return Key::T;
        case SDL_SCANCODE_U: return Key::U;
        // Both rows, because a number is a number wherever it is on the keyboard.
        case SDL_SCANCODE_1: case SDL_SCANCODE_KP_1: return Key::Digit1;
        case SDL_SCANCODE_2: case SDL_SCANCODE_KP_2: return Key::Digit2;
        case SDL_SCANCODE_3: case SDL_SCANCODE_KP_3: return Key::Digit3;
        case SDL_SCANCODE_4: case SDL_SCANCODE_KP_4: return Key::Digit4;
        case SDL_SCANCODE_5: case SDL_SCANCODE_KP_5: return Key::Digit5;
        case SDL_SCANCODE_6: case SDL_SCANCODE_KP_6: return Key::Digit6;
        case SDL_SCANCODE_7: case SDL_SCANCODE_KP_7: return Key::Digit7;
        case SDL_SCANCODE_8: case SDL_SCANCODE_KP_8: return Key::Digit8;
        case SDL_SCANCODE_9: case SDL_SCANCODE_KP_9: return Key::Digit9;
        case SDL_SCANCODE_LEFT:  return Key::Left;
        case SDL_SCANCODE_RIGHT: return Key::Right;
        case SDL_SCANCODE_UP:    return Key::Up;
        case SDL_SCANCODE_DOWN:  return Key::Down;
        case SDL_SCANCODE_COMMA:  case SDL_SCANCODE_KP_MINUS: return Key::Comma;
        case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_KP_PLUS:  return Key::Period;
        case SDL_SCANCODE_SLASH:  case SDL_SCANCODE_KP_DIVIDE: return Key::Slash;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return Key::Shift;
        case SDL_SCANCODE_LCTRL:  case SDL_SCANCODE_RCTRL:  return Key::Ctrl;
        case SDL_SCANCODE_LALT:   case SDL_SCANCODE_RALT:   return Key::Alt;
        case SDL_SCANCODE_F1:  return Key::F1;
        case SDL_SCANCODE_F2:  return Key::F2;
        case SDL_SCANCODE_F3:  return Key::F3;
        case SDL_SCANCODE_F4:  return Key::F4;
        case SDL_SCANCODE_F5:  return Key::F5;
        case SDL_SCANCODE_F8:  return Key::F8;
        case SDL_SCANCODE_F9:  return Key::F9;
        case SDL_SCANCODE_F10: return Key::F10;
        case SDL_SCANCODE_F11: return Key::F11;
        case SDL_SCANCODE_F12: return Key::F12;
        default: return Key::Unknown;
    }
}

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(const std::string& title, u32 width, u32 height, bool allow_oversize) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        WS_LOG_FATAL("window", "SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    // The size handed to SDL_CreateWindow is in *screen coordinates*, not pixels. With
    // display scaling those are not the same thing: at 150%, a 1920x1080 monitor is 1280x720
    // screen coordinates, so asking for a 1600x900 window puts most of it off the screen —
    // including the corners the interface anchors itself to. Clamped to what the desktop
    // actually has room for, leaving the taskbar alone.
    int requested_w = static_cast<int>(width);
    int requested_h = static_cast<int>(height);
    SDL_Rect usable{};
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (!allow_oversize && display != 0 && SDL_GetDisplayUsableBounds(display, &usable) &&
        usable.w > 0 && usable.h > 0) {
        // A margin for the window's own frame, which is outside the client area SDL sizes.
        const int limit_w = usable.w - 16;
        const int limit_h = usable.h - 48;
        if (requested_w > limit_w || requested_h > limit_h) {
            // Kept at the requested aspect ratio; a window that fits but is the wrong shape
            // would make every screenshot comparison against a previous build meaningless.
            const f64 scale = std::min(static_cast<f64>(limit_w) / requested_w,
                                       static_cast<f64>(limit_h) / requested_h);
            const int fitted_w = std::max(640, static_cast<int>(requested_w * scale));
            const int fitted_h = std::max(360, static_cast<int>(requested_h * scale));
            WS_LOG_INFO("window",
                        "{}x{} does not fit the desktop's {}x{} usable area; using {}x{}",
                        requested_w, requested_h, usable.w, usable.h, fitted_w, fitted_h);
            requested_w = fitted_w;
            requested_h = fitted_h;
        }
    }

    const SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(title.c_str(), requested_w, requested_h, flags);
    if (window_ == nullptr) {
        WS_LOG_FATAL("window", "SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    width_ = static_cast<u32>(w);
    height_ = static_cast<u32>(h);

    int points_w = 0;
    int points_h = 0;
    SDL_GetWindowSize(window_, &points_w, &points_h);
    WS_LOG_INFO("window", "created {}x{} pixels ({}x{} screen coordinates, SDL {}.{}.{})",
                width_, height_, points_w, points_h, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    return true;
}

void Window::destroy() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

bool Window::pump() {
    // Edge-triggered state is per frame; level-triggered state persists.
    std::memset(input_.pressed, 0, sizeof(input_.pressed));
    std::memset(input_.released, 0, sizeof(input_.released));
    input_.mouse_dx = 0.0f;
    input_.mouse_dy = 0.0f;
    input_.wheel = 0.0f;
    resized_ = false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event_hook_ != nullptr) event_hook_(event);

        switch (event.type) {
            case SDL_EVENT_QUIT:
                should_close_ = true;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                should_close_ = true;
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                width_ = static_cast<u32>(event.window.data1);
                height_ = static_cast<u32>(event.window.data2);
                resized_ = true;
                break;
            }

            case SDL_EVENT_WINDOW_MINIMIZED: minimised_ = true; break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED: minimised_ = false; break;

            case SDL_EVENT_KEY_DOWN: {
                const Key k = key_from_scancode(event.key.scancode);
                if (k != Key::Unknown && !event.key.repeat) {
                    input_.down[static_cast<usize>(k)] = true;
                    input_.pressed[static_cast<usize>(k)] = true;
                }
                break;
            }

            case SDL_EVENT_KEY_UP: {
                const Key k = key_from_scancode(event.key.scancode);
                if (k != Key::Unknown) {
                    input_.down[static_cast<usize>(k)] = false;
                    input_.released[static_cast<usize>(k)] = true;
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                input_.mouse_x = event.motion.x;
                input_.mouse_y = event.motion.y;
                input_.mouse_dx += event.motion.xrel;
                input_.mouse_dy += event.motion.yrel;
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                input_.wheel += event.wheel.y;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool is_down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                if (event.button.button == SDL_BUTTON_LEFT)   input_.mouse_left = is_down;
                if (event.button.button == SDL_BUTTON_RIGHT)  input_.mouse_right = is_down;
                if (event.button.button == SDL_BUTTON_MIDDLE) input_.mouse_middle = is_down;
                break;
            }

            default:
                break;
        }
    }

    return !should_close_;
}

void Window::set_relative_mouse(bool enabled) {
    if (window_ == nullptr) return;
    SDL_SetWindowRelativeMouseMode(window_, enabled);
    relative_mouse_ = enabled;
}

void Window::set_title(const std::string& title) {
    if (window_ != nullptr) SDL_SetWindowTitle(window_, title.c_str());
}

bool Window::create_vulkan_surface(void* instance, void** out_surface) const {
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(instance), nullptr,
                                  &surface)) {
        WS_LOG_FATAL("window", "SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return false;
    }
    *out_surface = surface;
    return true;
}

std::string Window::base_path() {
    // Owned by SDL for the life of the process; there is nothing to free.
    const char* path = SDL_GetBasePath();
    return (path != nullptr) ? std::string(path) : std::string();
}

const char* const* Window::required_vulkan_extensions(u32& count) {
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    count = static_cast<u32>(n);
    return names;
}

}  // namespace ws
