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
#include <format>
#include <iostream>
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

// Name of a window station or desktop handle, for logging and for noticing
// that the input desktop has changed.
inline std::string user_object_name(HANDLE object) {
    if (object == nullptr) {
        return "<none>";
    }
    char name[256]{};
    DWORD needed = 0;
    if (!::GetUserObjectInformationA(object, UOI_NAME, name, sizeof(name),
                                     &needed)) {
        return "<unknown>";
    }
    return std::string{name};
}

inline std::string current_window_station_name() {
    return user_object_name(::GetProcessWindowStation());
}

inline std::string current_desktop_name() {
    return user_object_name(::GetThreadDesktop(::GetCurrentThreadId()));
}

// Moves the process onto the interactive window station. A process launched by
// a service - PsExec -i, PSEXESVC, a scheduled task - inherits the service
// window station (Service-0x0-3e7$), whose only desktop is not the one the user
// sees, so OpenInputDesktop there fails with ERROR_ACCESS_DENIED.
inline bool attach_process_to_interactive_window_station() {
    // Never closed on purpose: the process keeps using it until it exits, and
    // closing a window station still in use is undefined.
    static HWINSTA attached = nullptr;
    if (attached != nullptr) {
        return true;
    }
    HWINSTA station = ::OpenWindowStationW(
        L"WinSta0", FALSE,
        WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES | WINSTA_ACCESSCLIPBOARD |
            WINSTA_CREATEDESKTOP | WINSTA_WRITEATTRIBUTES |
            WINSTA_ACCESSGLOBALATOMS | WINSTA_EXITWINDOWS | WINSTA_ENUMERATE |
            WINSTA_READSCREEN);
    if (station == nullptr) {
        return false;
    }
    if (!::SetProcessWindowStation(station)) {
        ::CloseWindowStation(station);
        return false;
    }
    attached = station;
    return true;
}

// Points the calling thread at the desktop that currently receives input.
// A process started outside the interactive shell - by PsExec -i, by a service,
// or by a scheduled task - starts out on some other desktop of the window
// station, where GetDC(nullptr) still succeeds but every BitBlt from it fails.
// The input desktop also changes when the session locks or a UAC prompt takes
// over, so this is re-run whenever a capture fails.

// What a thread that only reads pixels needs on its desktop.
inline constexpr DWORD desktop_access_capture =
    DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
    DESKTOP_ENUMERATE;

// What a thread that injects input needs. SendInput requires
// DESKTOP_JOURNALPLAYBACK on the thread's desktop and fails with
// ERROR_ACCESS_DENIED without it - silently, since it reports failure only in
// its return value. It is also the right most likely to be refused: a plain
// user token cannot get it on the secure desktop, which is why it is not asked
// for unconditionally and why the caller falls back to the capture-only mask.
inline constexpr DWORD desktop_access_input =
    desktop_access_capture | DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD;

inline bool attach_thread_to_input_desktop(
    HDESK& current, DWORD access = desktop_access_capture) {
    HDESK desktop = ::OpenInputDesktop(0, FALSE, access);
    if (desktop == nullptr && ::GetLastError() == ERROR_ACCESS_DENIED &&
        attach_process_to_interactive_window_station()) {
        desktop = ::OpenInputDesktop(0, FALSE, access);
    }
    // Injection rights were refused, so settle for the rights capture needs:
    // showing the desktop read-only beats failing outright.
    if (desktop == nullptr && access != desktop_access_capture) {
        desktop = ::OpenInputDesktop(0, FALSE, desktop_access_capture);
    }
    if (desktop == nullptr) {
        return false;
    }
    if (!::SetThreadDesktop(desktop)) {
        ::CloseDesktop(desktop);
        return false;
    }
    // The old handle can only be closed once no thread is using it any more,
    // which SetThreadDesktop above has just guaranteed.
    if (current != nullptr) {
        ::CloseDesktop(current);
    }
    current = desktop;
    return true;
}

// Name of the desktop that is currently receiving input, or empty when that
// cannot be determined (a plain user token cannot open the secure desktop).
inline std::string input_desktop_name() {
    HDESK desktop = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr && ::GetLastError() == ERROR_ACCESS_DENIED &&
        attach_process_to_interactive_window_station()) {
        desktop = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    }
    if (desktop == nullptr) {
        return {};
    }
    auto name = user_object_name(desktop);
    ::CloseDesktop(desktop);
    return name;
}

// Keeps the calling thread on whichever desktop is currently receiving input.
//
// The input desktop changes at logon and logoff, when the session locks, and
// for every UAC prompt. Nothing reports that to a thread already attached
// elsewhere, and the failure is silent rather than loud: GDI capture from the
// stale desktop keeps succeeding and simply returns the wrong screen - at the
// logon prompt it hands back WinSta0\Default's bare wallpaper - while
// SendInput delivers to a desktop nobody is looking at. Waiting for a capture
// to fail therefore never triggers, so the input desktop is polled by name and
// the thread follows it.
//
// Returns true when the thread was actually moved, which invalidates every DC
// the caller holds: a DC belongs to the desktop it was created on.
inline bool follow_input_desktop(HDESK& current, std::string& attached_name,
                                 DWORD access = desktop_access_capture) {
    auto name = input_desktop_name();
    if (name.empty() || name == attached_name) {
        return false;
    }
    if (!attach_thread_to_input_desktop(current, access)) {
        return false;
    }
    attached_name = current_desktop_name();
    return true;
}

class screen_capture {
public:
    explicit screen_capture(const capture_options& options)
        : m_options{options} {
        make_process_dpi_aware();
        // Best effort: this fails when the process runs in a session with no
        // desktop at all, in which case capture() reports the failure instead.
        bool attached = attach_thread_to_input_desktop(m_desktop);
        m_desktop_name = current_desktop_name();
        std::cerr << std::format(
            "capture context: window station '{}', desktop '{}', input desktop "
            "attach {}\n",
            current_window_station_name(), m_desktop_name,
            attached ? "ok"
                     : std::format("failed (error {})", ::GetLastError()));
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
        release_gdi();
        if (m_desktop != nullptr) {
            ::CloseDesktop(m_desktop);
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
        // The input desktop can change without anything here failing, so it is
        // checked before the blit rather than after one goes wrong. Following
        // it invalidates the DCs, which belong to the old desktop.
        if (follow_input_desktop(m_desktop, m_desktop_name)) {
            std::cerr << std::format("capture: following input desktop '{}'\n",
                                     m_desktop_name);
            if (!rebuild_gdi()) {
                return false;
            }
        }
        // CAPTUREBLT includes layered windows, which is most of the modern
        // Windows 11 shell.
        if (!blit()) {
            DWORD blit_error = ::GetLastError();
            // Either the input desktop changed under us (lock screen, UAC) or
            // this process was never on it to begin with. Re-attach and retry
            // once; the DCs belong to the old desktop, so they go too.
            if (!attach_thread_to_input_desktop(m_desktop)) {
                m_diagnostic = std::format(
                    "BitBlt failed (error {}) and OpenInputDesktop/"
                    "SetThreadDesktop failed (error {})",
                    blit_error, ::GetLastError());
                return false;
            }
            m_desktop_name = current_desktop_name();
            if (!rebuild_gdi()) {
                return false;
            }
            if (!blit()) {
                m_diagnostic = std::format(
                    "BitBlt failed (error {}) even after re-attaching to "
                    "input desktop '{}'",
                    ::GetLastError(), current_desktop_name());
                return false;
            }
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
        m_diagnostic.clear();
        return true;
    }

    // Why the last capture() failed, for logging. Empty after a good capture.
    const std::string& diagnostic() const { return m_diagnostic; }

private:
    // Drops the DCs and takes new ones on whatever desktop the thread is now
    // attached to. The screen geometry is re-read as well: the logon desktop
    // and the user's desktop need not be the same size.
    bool rebuild_gdi() {
        release_gdi();
        m_screen_dc = ::GetDC(nullptr);
        if (m_screen_dc == nullptr) {
            m_diagnostic = std::format(
                "GetDC(nullptr) failed after re-attaching to the input "
                "desktop (error {})",
                ::GetLastError());
            return false;
        }
        query_geometry();
        create_dib();
        return true;
    }

    bool blit() {
        return ::BitBlt(m_memory_dc, 0, 0, m_width, m_height, m_screen_dc,
                        m_origin_x, m_origin_y, SRCCOPY | CAPTUREBLT) != FALSE;
    }

    void release_gdi() {
        if (m_memory_dc != nullptr) {
            ::SelectObject(m_memory_dc, m_old_bitmap);
            ::DeleteDC(m_memory_dc);
            m_memory_dc = nullptr;
        }
        if (m_bitmap != nullptr) {
            ::DeleteObject(m_bitmap);
            m_bitmap = nullptr;
        }
        if (m_screen_dc != nullptr) {
            ::ReleaseDC(nullptr, m_screen_dc);
            m_screen_dc = nullptr;
        }
        m_bits = nullptr;
    }

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
    std::string m_diagnostic{};
    HDESK m_desktop{};
    std::string m_desktop_name{};
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
    input_injector(const input_injector&) = delete;
    input_injector& operator=(const input_injector&) = delete;
    ~input_injector() {
        if (m_desktop != nullptr) {
            ::CloseDesktop(m_desktop);
        }
    }

    void pointer_event(uint8_t button_mask, int x, int y) {
        follow_desktop();
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
        follow_desktop();
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
    // SendInput delivers to the desktop the calling thread is attached to, and
    // this thread starts on whichever desktop the process was launched with.
    // Without following the input desktop, every event injected while the
    // logon screen, the lock screen or a UAC prompt is up goes to a desktop
    // nobody is looking at, and the session appears to ignore all input.
    void follow_desktop() {
        if (follow_input_desktop(m_desktop, m_desktop_name,
                                 desktop_access_input)) {
            std::cerr << std::format("input: following input desktop '{}'\n",
                                     m_desktop_name);
            m_send_failures = 0;
        }
    }

    // SendInput silently does nothing when the calling thread is not on the
    // desktop receiving input, and reports ERROR_ACCESS_DENIED when UIPI
    // blocks the injection - a server that is not elevated cannot drive an
    // elevated window, and one that is not LocalSystem cannot touch the secure
    // desktop at all. Neither shows up anywhere unless the result is checked,
    // which is why an ignored return value made "the session ignores my mouse"
    // impossible to tell apart from a dead connection.
    void submit(const INPUT& input) {
        if (::SendInput(1, const_cast<INPUT*>(&input), sizeof(INPUT)) != 0) {
            m_send_failures = 0;
            return;
        }
        DWORD error = ::GetLastError();
        // One line per run of failures: a viewer dragging the pointer would
        // otherwise fill the log with thousands of identical messages.
        if (m_send_failures == 0) {
            std::cerr << std::format(
                "input: SendInput failed (error {}) on desktop '{}'\n", error,
                current_desktop_name());
        }
        m_send_failures++;
    }

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
        submit(input);
    }

    void send_mouse(DWORD flags, int data) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        input.mi.mouseData = static_cast<DWORD>(data);
        submit(input);
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
        submit(input);
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
        submit(input);
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

    HDESK m_desktop{};
    std::string m_desktop_name{};
    unsigned m_send_failures{};
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

// Boot-time supervisor.
//
// A desktop can only be captured from a process inside that desktop's session,
// but anything that starts at boot - a service, or a scheduled task with an
// "At startup" trigger - runs in session 0, which has no desktop. Logon items
// are in the right session but only start after somebody logs in.
//
// This bridges the two: it runs in session 0 as LocalSystem, and launches the
// real server into whatever session is currently attached to the console,
// which exists from boot onwards (showing the logon screen until someone signs
// in). The worker is restarted if it exits or if the console session changes,
// e.g. on Fast User Switching.
class console_session_launcher {
public:
    console_session_launcher(std::wstring executable, std::wstring command_line)
        : m_executable{std::move(executable)},
          m_command_line{std::move(command_line)} {}

    // Never returns under normal operation.
    int run() {
        for (;;) {
            DWORD session = ::WTSGetActiveConsoleSessionId();
            if (session == 0xFFFFFFFF) {
                // No console session at all: the machine is mid-transition.
                std::cout << "no active console session, waiting\n";
                ::Sleep(retry_delay_ms);
                continue;
            }
            if (!launch_worker(session)) {
                ::Sleep(retry_delay_ms);
                continue;
            }
            supervise(session);
        }
    }

private:
    static constexpr DWORD retry_delay_ms = 2000;
    static constexpr DWORD poll_interval_ms = 1000;

    bool launch_worker(DWORD session) {
        HANDLE own_token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
                                &own_token)) {
            std::cout << std::format("OpenProcessToken failed (error {})\n",
                                     ::GetLastError());
            return false;
        }
        HANDLE session_token = nullptr;
        BOOL duplicated = ::DuplicateTokenEx(own_token, MAXIMUM_ALLOWED, nullptr,
                                             SecurityImpersonation, TokenPrimary,
                                             &session_token);
        ::CloseHandle(own_token);
        if (!duplicated) {
            std::cout << std::format("DuplicateTokenEx failed (error {})\n",
                                     ::GetLastError());
            return false;
        }
        // Moving the token into the console session is what makes the child
        // land on a real desktop instead of session 0. It needs the
        // SE_TCB_NAME privilege, which LocalSystem has and nobody else does.
        if (!::SetTokenInformation(session_token, TokenSessionId, &session,
                                   sizeof(session))) {
            std::cout << std::format(
                "SetTokenInformation(TokenSessionId={}) failed (error {}); the "
                "launcher must run as LocalSystem\n",
                session, ::GetLastError());
            ::CloseHandle(session_token);
            return false;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // Named explicitly so the child starts on the interactive desktop even
        // though the launcher itself has none.
        std::wstring desktop = L"WinSta0\\Default";
        startup.lpDesktop = desktop.data();
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        // Hand the launcher's own stdout and stderr to the worker. Everything
        // that matters for diagnosing a boot-time server - which desktop it
        // attached to, capture failures, the encoder it picked - is written by
        // the worker, and without this it goes nowhere at all: the log the task
        // redirects into only ever holds the launcher's own few lines.
        BOOL inherit_handles = FALSE;
        HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
        if (out != nullptr && out != INVALID_HANDLE_VALUE) {
            ::SetHandleInformation(out, HANDLE_FLAG_INHERIT,
                                   HANDLE_FLAG_INHERIT);
            if (err == nullptr || err == INVALID_HANDLE_VALUE) {
                err = out;
            }
            else {
                ::SetHandleInformation(err, HANDLE_FLAG_INHERIT,
                                       HANDLE_FLAG_INHERIT);
            }
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdInput = nullptr;
            startup.hStdOutput = out;
            startup.hStdError = err;
            inherit_handles = TRUE;
        }

        PROCESS_INFORMATION process{};
        std::wstring command_line = m_command_line;
        BOOL created = ::CreateProcessAsUserW(
            session_token, m_executable.c_str(), command_line.data(), nullptr,
            nullptr, inherit_handles, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr, nullptr, &startup, &process);
        DWORD create_error = ::GetLastError();
        ::CloseHandle(session_token);
        if (!created) {
            std::cout << std::format(
                "CreateProcessAsUser into session {} failed (error {})\n",
                session, create_error);
            return false;
        }
        ::CloseHandle(process.hThread);
        m_worker = process.hProcess;
        std::cout << std::format("started worker pid {} in session {}\n",
                                 process.dwProcessId, session);
        return true;
    }

    // Waits for the worker to exit, or for the console session to move
    // somewhere else, in which case the worker is on a dead desktop and has to
    // be replaced.
    void supervise(DWORD session) {
        for (;;) {
            DWORD waited = ::WaitForSingleObject(m_worker, poll_interval_ms);
            if (waited == WAIT_OBJECT_0) {
                DWORD code = 0;
                ::GetExitCodeProcess(m_worker, &code);
                ::CloseHandle(m_worker);
                m_worker = nullptr;
                std::cout << std::format("worker exited with code {}, restarting\n",
                                         code);
                ::Sleep(retry_delay_ms);
                return;
            }
            if (::WTSGetActiveConsoleSessionId() != session) {
                std::cout << "console session changed, restarting worker\n";
                ::TerminateProcess(m_worker, 0);
                ::WaitForSingleObject(m_worker, 5000);
                ::CloseHandle(m_worker);
                m_worker = nullptr;
                return;
            }
        }
    }

    std::wstring m_executable;
    std::wstring m_command_line;
    HANDLE m_worker{};
};

// Full path of the running executable, so the launcher can re-run itself.
inline std::wstring own_executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD written = ::GetModuleFileNameW(nullptr, path.data(),
                                             static_cast<DWORD>(path.size()));
        if (written == 0) {
            throw std::runtime_error{"GetModuleFileName fail"};
        }
        if (written < path.size()) {
            path.resize(written);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

inline std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Builds a command line for CreateProcess out of already-parsed arguments.
// Quoting follows the rules CommandLineToArgvW undoes.
inline std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (size_t i = 0; i < argument.size(); i++) {
        size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            backslashes++;
            i++;
        }
        if (i == argument.size()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        }
        else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(argument[i]);
    }
    quoted.push_back(L'"');
    return quoted;
}

// Runs the console-session launcher for a worker started with worker_args.
inline int run_console_launcher(const std::vector<std::string>& worker_args) {
    auto executable = own_executable_path();
    std::wstring command_line = quote_argument(executable);
    for (const auto& argument : worker_args) {
        command_line.push_back(L' ');
        command_line.append(quote_argument(widen(argument)));
    }
    console_session_launcher launcher{executable, command_line};
    return launcher.run();
}

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

    const std::string& diagnostic() const { return m_diagnostic; }

private:
    std::string m_diagnostic{};

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

// Sessions and desktops in the Windows sense do not exist here, so there is
// nothing to bridge: an init system starts the server directly.
inline int run_console_launcher(const std::vector<std::string>&) {
    throw std::runtime_error{
        "--console-launcher is a Windows-only mechanism; on this platform run "
        "the server directly from your init system"};
}

#endif // _WIN32

} // namespace rfb::server
