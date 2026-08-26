#pragma once

// Platform side of the RFB server: where the pixels come from and where the
// input events go. Windows 11 is the target, so that path captures the real
// desktop through GDI and replays events through SendInput. Every other
// platform gets a synthetic animated framebuffer, which keeps the protocol
// code buildable and testable with an ordinary VNC viewer on Linux.

#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "keysym.hpp"

namespace rfb::server {

// The framebuffer the server works in internally: 32 bits per pixel, one byte
// each of blue, green, red, unused, in memory order. That is what a Windows
// BI_RGB DIB section hands back, so capture is a straight copy.
struct frame_buffer {
    int width{};
    int height{};
    std::vector<uint8_t> pixels{};

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    }
    size_t stride() const { return static_cast<size_t>(width) * 4; }
};

struct capture_options {
    // Capture every monitor as one big framebuffer instead of just the primary
    // one.
    bool all_monitors{false};
    // Composite the mouse cursor into the framebuffer. RFB can also ship the
    // cursor as a pseudo-encoding, but drawing it works with every viewer.
    bool draw_cursor{true};
};

#ifdef _WIN32

// Reports real pixels rather than the virtualised, DPI-scaled coordinates a
// non-aware process sees. Without this a 150% scaled 4K desktop is captured
// as a blurry 2560x1440 image.
inline void make_process_dpi_aware() {
    using set_ctx_t = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto set_ctx = reinterpret_cast<set_ctx_t>(
            reinterpret_cast<void*>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (set_ctx != nullptr &&
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    ::SetProcessDPIAware();
}

class screen_capture {
public:
    explicit screen_capture(const capture_options& options)
        : m_options{options} {
        make_process_dpi_aware();
        m_screen_dc = ::GetDC(nullptr);
        if (m_screen_dc == nullptr) {
            throw std::runtime_error{"GetDC(nullptr) fail"};
        }
        query_geometry();
        create_dib();
    }
    screen_capture(const screen_capture&) = delete;
    screen_capture& operator=(const screen_capture&) = delete;
    ~screen_capture() {
        if (m_memory_dc != nullptr) {
            ::SelectObject(m_memory_dc, m_old_bitmap);
            ::DeleteDC(m_memory_dc);
        }
        if (m_bitmap != nullptr) {
            ::DeleteObject(m_bitmap);
        }
        if (m_screen_dc != nullptr) {
            ::ReleaseDC(nullptr, m_screen_dc);
        }
    }

    int width() const { return m_width; }
    int height() const { return m_height; }
    // Top left of the captured area in virtual screen coordinates; pointer
    // events arrive relative to the framebuffer and have to be shifted back.
    int origin_x() const { return m_origin_x; }
    int origin_y() const { return m_origin_y; }

    // Copies the current desktop into frame. Returns false when the desktop
    // could not be read this time round, e.g. because the secure desktop
    // (UAC prompt, lock screen) is in front, which is not fatal.
    bool capture(frame_buffer& frame) {
        // CAPTUREBLT includes layered windows, which is most of the modern
        // Windows 11 shell.
        if (!::BitBlt(m_memory_dc, 0, 0, m_width, m_height, m_screen_dc,
                      m_origin_x, m_origin_y, SRCCOPY | CAPTUREBLT)) {
            return false;
        }
        if (m_options.draw_cursor) {
            draw_cursor();
        }
        if (frame.width != m_width || frame.height != m_height) {
            frame.resize(m_width, m_height);
        }
        // The DIB is bottom-up-free (negative height), so its rows already
        // match the framebuffer order.
        std::memcpy(frame.pixels.data(), m_bits, frame.pixels.size());
        return true;
    }

private:
    void query_geometry() {
        if (m_options.all_monitors) {
            m_origin_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
            m_origin_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
            m_width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
            m_height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        }
        else {
            m_origin_x = 0;
            m_origin_y = 0;
            m_width = ::GetSystemMetrics(SM_CXSCREEN);
            m_height = ::GetSystemMetrics(SM_CYSCREEN);
        }
        if (m_width <= 0 || m_height <= 0) {
            throw std::runtime_error{"screen size query fail"};
        }
    }

    void create_dib() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = m_width;
        // Negative height requests a top-down DIB, matching RFB's row order.
        info.bmiHeader.biHeight = -m_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        m_bitmap = ::CreateDIBSection(m_screen_dc, &info, DIB_RGB_COLORS,
                                      &m_bits, nullptr, 0);
        if (m_bitmap == nullptr || m_bits == nullptr) {
            throw std::runtime_error{"CreateDIBSection fail"};
        }
        m_memory_dc = ::CreateCompatibleDC(m_screen_dc);
        if (m_memory_dc == nullptr) {
            throw std::runtime_error{"CreateCompatibleDC fail"};
        }
        m_old_bitmap = ::SelectObject(m_memory_dc, m_bitmap);
    }

    void draw_cursor() {
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        if (!::GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) == 0) {
            return;
        }
        ICONINFO icon{};
        if (!::GetIconInfo(cursor.hCursor, &icon)) {
            return;
        }
        int x = cursor.ptScreenPos.x - m_origin_x - static_cast<int>(icon.xHotspot);
        int y = cursor.ptScreenPos.y - m_origin_y - static_cast<int>(icon.yHotspot);
        ::DrawIconEx(m_memory_dc, x, y, cursor.hCursor, 0, 0, 0, nullptr,
                     DI_NORMAL);
        if (icon.hbmColor != nullptr) ::DeleteObject(icon.hbmColor);
        if (icon.hbmMask != nullptr) ::DeleteObject(icon.hbmMask);
    }

    capture_options m_options;
    HDC m_screen_dc{};
    HDC m_memory_dc{};
    HBITMAP m_bitmap{};
    HGDIOBJ m_old_bitmap{};
    void* m_bits{};
    int m_width{};
    int m_height{};
    int m_origin_x{};
    int m_origin_y{};
};

// Replays client input on the local desktop. SendInput is subject to UIPI:
// to drive elevated windows the server itself has to run elevated.
class input_injector {
public:
    input_injector(int origin_x, int origin_y)
        : m_origin_x{origin_x}, m_origin_y{origin_y} {}

    void pointer_event(uint8_t button_mask, int x, int y) {
        move_pointer(x, y);

        static constexpr struct {
            uint8_t bit;
            DWORD down;
            DWORD up;
        } buttons[] = {
            {0x01, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP},
            {0x02, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP},
            {0x04, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP},
        };
        for (const auto& button : buttons) {
            bool was_down = (m_button_mask & button.bit) != 0;
            bool is_down = (button_mask & button.bit) != 0;
            if (was_down != is_down) {
                send_mouse(is_down ? button.down : button.up, 0);
            }
        }

        // Buttons 4/5 and 6/7 are the vertical and horizontal wheel: a click
        // of each is reported as a press immediately followed by a release,
        // so only the press edge should turn into a scroll.
        auto pressed = static_cast<uint8_t>(button_mask & ~m_button_mask);
        if (pressed & 0x08) send_mouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA);
        if (pressed & 0x10) send_mouse(MOUSEEVENTF_WHEEL, -WHEEL_DELTA);
        if (pressed & 0x20) send_mouse(MOUSEEVENTF_HWHEEL, -WHEEL_DELTA);
        if (pressed & 0x40) send_mouse(MOUSEEVENTF_HWHEEL, WHEEL_DELTA);

        m_button_mask = button_mask;
    }

    void key_event(uint32_t keysym, bool down) {
        if (is_control_keysym(keysym)) m_control_down = down;
        if (is_alt_keysym(keysym)) m_alt_down = down;

        auto vk = keysym_to_virtual_key(keysym);
        if (vk != 0) {
            send_key_vk(vk, down);
            return;
        }
        auto code_point = keysym_to_unicode(keysym);
        if (code_point == 0) {
            return;
        }
        // KEYEVENTF_UNICODE bypasses the keyboard layout, which is exactly
        // right for typing text and exactly wrong for shortcuts: Windows
        // derives Ctrl+C from the virtual key, so an injected literal 'c'
        // with Ctrl held does nothing. Fall back to a layout lookup while a
        // modifier is held.
        if ((m_control_down || m_alt_down) && code_point <= 0xffff) {
            auto scan = ::VkKeyScanW(static_cast<wchar_t>(code_point));
            if (scan != -1) {
                send_key_vk(static_cast<WORD>(LOBYTE(scan)), down);
                return;
            }
        }
        send_key_unicode(code_point, down);
    }

    // Releases whatever the client left held when it disconnects, so a
    // dropped connection cannot leave Ctrl or a mouse button stuck down.
    void release_all() {
        pointer_event(0, m_last_x, m_last_y);
        auto held = m_held_keys;
        for (auto vk : held) {
            send_key_vk(vk, false);
        }
        m_held_keys.clear();
        m_control_down = false;
        m_alt_down = false;
    }

private:
    void move_pointer(int x, int y) {
        m_last_x = x;
        m_last_y = y;
        int virtual_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtual_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtual_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtual_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (virtual_w <= 1 || virtual_h <= 1) {
            return;
        }
        // MOUSEEVENTF_ABSOLUTE coordinates are 0..65535 across the whole
        // virtual desktop, and the -1 divisor is what makes the last pixel
        // reachable.
        int screen_x = x + m_origin_x - virtual_x;
        int screen_y = y + m_origin_y - virtual_y;
        auto normalized_x = static_cast<LONG>(
            (static_cast<int64_t>(screen_x) * 65535) / (virtual_w - 1));
        auto normalized_y = static_cast<LONG>(
            (static_cast<int64_t>(screen_y) * 65535) / (virtual_h - 1));
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = normalized_x;
        input.mi.dy = normalized_y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                           MOUSEEVENTF_VIRTUALDESK;
        ::SendInput(1, &input, sizeof(input));
    }

    void send_mouse(DWORD flags, int data) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        input.mi.mouseData = static_cast<DWORD>(data);
        ::SendInput(1, &input, sizeof(input));
    }

    void send_key_vk(WORD vk, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        if (is_extended_key(vk)) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        ::SendInput(1, &input, sizeof(input));
        if (down) {
            m_held_keys.insert(vk);
        }
        else {
            m_held_keys.erase(vk);
        }
    }

    void send_key_unicode(uint32_t code_point, bool down) {
        // Outside the BMP a code point needs two surrogate events.
        if (code_point > 0xffff) {
            code_point -= 0x10000;
            send_key_unit(static_cast<WORD>(0xd800 + (code_point >> 10)), down);
            send_key_unit(static_cast<WORD>(0xdc00 + (code_point & 0x3ff)), down);
            return;
        }
        send_key_unit(static_cast<WORD>(code_point), down);
    }

    void send_key_unit(WORD unit, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = unit;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
        ::SendInput(1, &input, sizeof(input));
    }

    static bool is_extended_key(WORD vk) {
        switch (vk) {
        case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_LWIN: case VK_RWIN:
        case VK_APPS: case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    }

    int m_origin_x{};
    int m_origin_y{};
    uint8_t m_button_mask{};
    int m_last_x{};
    int m_last_y{};
    bool m_control_down{false};
    bool m_alt_down{false};
    std::set<WORD> m_held_keys{};
};

// Text the client copied is pushed into the Windows clipboard, and anything
// copied locally is offered back to the client.
class clipboard {
public:
    void set_text(const std::string& text) {
        auto wide = to_wide(text);
        if (!::OpenClipboard(nullptr)) {
            return;
        }
        ::EmptyClipboard();
        auto bytes = (wide.size() + 1) * sizeof(wchar_t);
        auto handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle != nullptr) {
            auto* dst = static_cast<wchar_t*>(::GlobalLock(handle));
            if (dst != nullptr) {
                std::memcpy(dst, wide.c_str(), bytes);
                ::GlobalUnlock(handle);
                if (::SetClipboardData(CF_UNICODETEXT, handle) != nullptr) {
                    // Ownership moved to the clipboard.
                    handle = nullptr;
                }
            }
            if (handle != nullptr) {
                ::GlobalFree(handle);
            }
        }
        ::CloseClipboard();
        m_last_sequence = ::GetClipboardSequenceNumber();
    }

    // Returns the clipboard text if it changed since the last call.
    bool poll_text(std::string& text) {
        auto sequence = ::GetClipboardSequenceNumber();
        if (sequence == m_last_sequence) {
            return false;
        }
        m_last_sequence = sequence;
        if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) ||
            !::OpenClipboard(nullptr)) {
            return false;
        }
        bool got_text = false;
        auto handle = ::GetClipboardData(CF_UNICODETEXT);
        if (handle != nullptr) {
            auto* src = static_cast<const wchar_t*>(::GlobalLock(handle));
            if (src != nullptr) {
                text = from_wide(src);
                got_text = true;
                ::GlobalUnlock(handle);
            }
        }
        ::CloseClipboard();
        return got_text;
    }

private:
    static std::wstring to_wide(const std::string& text) {
        if (text.empty()) return {};
        int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }
    static std::string from_wide(const wchar_t* text) {
        int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
        if (size <= 1) return {};
        std::string narrow(static_cast<size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size,
                              nullptr, nullptr);
        return narrow;
    }

    DWORD m_last_sequence{};
};

#else // !_WIN32

// There is no portable desktop capture outside Windows, so the fallback draws
// a moving test pattern instead. It exercises the whole encode/transmit path,
// which is what the Linux build is for.
class screen_capture {
public:
    explicit screen_capture(const capture_options&)
        : m_start{std::chrono::steady_clock::now()} {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    int origin_x() const { return 0; }
    int origin_y() const { return 0; }

    bool capture(frame_buffer& frame) {
        if (frame.width != m_width || frame.height != m_height) {
            frame.resize(m_width, m_height);
        }
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_start).count();
        auto phase = static_cast<int>(elapsed * 60.0);
        for (int y = 0; y < m_height; y++) {
            auto* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride();
            for (int x = 0; x < m_width; x++) {
                // Large flat bands so the palette and run-length paths of the
                // ZRLE encoder both get used.
                uint8_t r = static_cast<uint8_t>(((x + phase) / 32) * 32);
                uint8_t g = static_cast<uint8_t>((y / 32) * 32);
                uint8_t b = static_cast<uint8_t>(phase);
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = g;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 0;
            }
        }
        draw_cursor(frame, phase);
        return true;
    }

private:
    void draw_cursor(frame_buffer& frame, int phase) {
        int cx = 40 + (phase * 3) % (m_width - 80);
        int cy = m_height / 2 + static_cast<int>(80 * std::sin(phase * 0.05));
        for (int y = cy - 12; y < cy + 12; y++) {
            if (y < 0 || y >= m_height) continue;
            auto* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride();
            for (int x = cx - 12; x < cx + 12; x++) {
                if (x < 0 || x >= m_width) continue;
                row[x * 4 + 0] = 255;
                row[x * 4 + 1] = 255;
                row[x * 4 + 2] = 255;
            }
        }
    }

    static constexpr int m_width = 1024;
    static constexpr int m_height = 768;
    std::chrono::steady_clock::time_point m_start;
};

class input_injector {
public:
    input_injector(int, int) {}
    void pointer_event(uint8_t, int, int) {}
    void key_event(uint32_t, bool) {}
    void release_all() {}
};

class clipboard {
public:
    void set_text(const std::string& text) { m_text = text; }
    bool poll_text(std::string&) { return false; }
private:
    std::string m_text;
};

#endif // _WIN32

} // namespace rfb::server
