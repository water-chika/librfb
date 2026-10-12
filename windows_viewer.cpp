#ifdef _WIN32

#include "control_server.hpp"
#include "trace.hpp"
#include "viewer_client.hpp"

#include <cpp_helper.hpp>
#include <socket_helper.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct config : cpp_helper::empty_configure {
    const char* address;
    std::uint16_t port;
    std::vector<std::uint32_t> supported_encodings;
    bool view_only;
    std::string password;
    std::uint16_t control_port;
};

using client_type = rfb_trace::add_trace_recorder<
    rfb::viewer_client<cpp_helper::empty_configurable_class>>;

std::uint16_t parse_port(const char* text, const char* option) {
    std::size_t used{};
    auto value = std::stoul(text, &used);
    if (text[used] != '\0' || value == 0 || value > 65535)
        throw std::logic_error{std::string{option} + " must be in 1..65535"};
    return static_cast<std::uint16_t>(value);
}

UINT message_scan_code(LPARAM lparam) {
    return static_cast<UINT>((lparam >> 16) & 0xff);
}

UINT map_scan_code(LPARAM lparam) {
    auto scan = message_scan_code(lparam);
    // MAPVK_VSC_TO_VK_EX accepts the E0 prefix in the high byte. Do not pass
    // this form to ToUnicodeEx: bit 15 there means that the key is being released.
    if (lparam & (1LL << 24)) scan |= 0xe000u;
    return scan;
}

UINT normalize_virtual_key(WPARAM vk, LPARAM lparam) {
    auto extended = (lparam & (1LL << 24)) != 0;
    if (vk == VK_SHIFT) {
        auto mapped = MapVirtualKeyW(map_scan_code(lparam), MAPVK_VSC_TO_VK_EX);
        return mapped ? mapped : VK_LSHIFT;
    }
    if (vk == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return static_cast<UINT>(vk);
}

std::uint32_t physical_key_id(WPARAM vk, LPARAM lparam) {
    auto scan = message_scan_code(lparam);
    auto extended = (lparam & (1LL << 24)) ? 0x100u : 0u;
    return scan | extended | (static_cast<std::uint32_t>(vk) << 16);
}

std::uint32_t virtual_key_to_keysym(UINT vk) {
    // Printable keys are translated below with ToUnicodeEx so Shift, Caps Lock
    // and the active keyboard layout are reflected in the keysym.
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return 0;
    if (vk >= VK_F1 && vk <= VK_F12) return 0xffbeu + static_cast<std::uint32_t>(vk - VK_F1);
    switch (vk) {
    case VK_BACK: return 0xff08; case VK_TAB: return 0xff09;
    case VK_RETURN: return 0xff0d; case VK_PAUSE: return 0xff13;
    case VK_ESCAPE: return 0xff1b; case VK_HOME: return 0xff50;
    case VK_LEFT: return 0xff51; case VK_UP: return 0xff52;
    case VK_RIGHT: return 0xff53; case VK_DOWN: return 0xff54;
    case VK_PRIOR: return 0xff55; case VK_NEXT: return 0xff56;
    case VK_END: return 0xff57; case VK_SNAPSHOT: return 0xff61;
    case VK_INSERT: return 0xff63; case VK_APPS: return 0xff67;
    case VK_DELETE: return 0xffff; case VK_NUMLOCK: return 0xff7f;
    case VK_CAPITAL: return 0xffe5; case VK_SCROLL: return 0xff14;
    case VK_MULTIPLY: return 0xffaa; case VK_ADD: return 0xffab;
    case VK_SUBTRACT: return 0xffad; case VK_DECIMAL: return 0xffae;
    case VK_DIVIDE: return 0xffaf;
    case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3:
    case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7:
    case VK_NUMPAD8: case VK_NUMPAD9:
        return 0xffb0u + static_cast<std::uint32_t>(vk - VK_NUMPAD0);
    case VK_LSHIFT: return 0xffe1;
    case VK_RSHIFT: return 0xffe2; case VK_SHIFT: return 0xffe1;
    case VK_LCONTROL: return 0xffe3; case VK_RCONTROL: return 0xffe4;
    case VK_CONTROL: return 0xffe3; case VK_LMENU: return 0xffe9;
    case VK_RMENU: return 0xffea; case VK_MENU: return 0xffe9;
    case VK_LWIN: return 0xffeb; case VK_RWIN: return 0xffec;
    case VK_SPACE: return ' ';
    default: return 0;
    }
}

std::string read_password_file(const char* path) {
    std::ifstream in{path};
    if (!in) throw std::runtime_error{"could not open password file"};
    std::string password;
    std::getline(in, password);
    if (!password.empty() && password.back() == '\r') password.pop_back();
    return password;
}

class win32_viewer {
public:
    explicit win32_viewer(config conf)
        : m_conf{std::move(conf)}, m_client{m_conf},
          m_control{m_conf.control_port, m_conf.address, m_conf.port, m_client} {
        register_window();
        create_window();
        create_d3d();
        m_client.request_framebuffer_update(0, 0, m_client.get_fb_width(),
                                            m_client.get_fb_height(), false);
        m_clipboard_sequence = GetClipboardSequenceNumber();
    }

    int run() {
        ShowWindow(m_window, SW_SHOWDEFAULT);
        UpdateWindow(m_window);
        while (m_running) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) m_running = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!m_running || !m_control.poll_once()) break;
            poll_clipboard();
            poll_rfb();
            // Sleep until GUI work arrives, with a short ceiling so socket work
            // remains responsive without spinning at 1 kHz while idle.
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
        }
        release_input();
        return 0;
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<win32_viewer*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<win32_viewer*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle_message(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CLOSE: DestroyWindow(m_window); return 0;
        case WM_DESTROY: m_running = false; PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            release_input(); return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_KEYUP: case WM_SYSKEYUP: {
            bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            auto physical = physical_key_id(wparam, lparam);
            auto normalized_vk = normalize_virtual_key(wparam, lparam);
            auto keysym = virtual_key_to_keysym(normalized_vk);
            if (!keysym && down) {
                BYTE state[256]{};
                wchar_t chars[4]{};
                if (GetKeyboardState(state)) {
                    auto count = ToUnicodeEx(normalized_vk,
                                             message_scan_code(lparam), state,
                                             chars, 4, 0,
                                             GetKeyboardLayout(0));
                    // A negative result is a dead key: ToUnicodeEx has updated
                    // the layout's composition state, but there is no character
                    // (and therefore no RFB keysym) to send for this event.
                    if (count == 1 || (count == 2 &&
                        IS_HIGH_SURROGATE(chars[0]) && IS_LOW_SURROGATE(chars[1]))) {
                        auto codepoint = count == 1
                            ? static_cast<std::uint32_t>(chars[0])
                            : static_cast<std::uint32_t>(
                                0x10000 + ((chars[0] - 0xd800) << 10) +
                                (chars[1] - 0xdc00));
                        keysym = codepoint <= 0xff ? codepoint
                                                  : 0x01000000u | codepoint;
                    }
                }
            }
            if (down) {
                if (keysym) m_physical_keysyms[physical] = keysym;
            } else {
                // Modifiers and layout state can change between the two messages.
                // Always release the exact keysym recorded for the down event;
                // an unrecorded event (including a dead key) sends no key-up.
                auto it = m_physical_keysyms.find(physical);
                if (it == m_physical_keysyms.end()) return 0;
                keysym = it->second;
                m_physical_keysyms.erase(it);
            }
            if (keysym) m_client.send_key_event(
                static_cast<int>(keysym), down ? 1 : 0);
            return 0;
        }
        case WM_MOUSEMOVE:
            send_pointer(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
        case WM_LBUTTONDOWN: set_button(0, true, lparam); return 0;
        case WM_LBUTTONUP: set_button(0, false, lparam); return 0;
        case WM_MBUTTONDOWN: set_button(1, true, lparam); return 0;
        case WM_MBUTTONUP: set_button(1, false, lparam); return 0;
        case WM_RBUTTONDOWN: set_button(2, true, lparam); return 0;
        case WM_RBUTTONUP: set_button(2, false, lparam); return 0;
        case WM_CAPTURECHANGED:
            if (m_buttons) {
                m_buttons = 0;
                m_client.release_remote_input();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            auto point = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(m_window, &point);
            auto delta = GET_WHEEL_DELTA_WPARAM(wparam);
            auto wheel = delta > 0 ? 3u : 4u;
            for (auto steps = std::abs(delta) / WHEEL_DELTA; steps > 0; --steps) {
                send_pointer(point.x, point.y, m_buttons | (1u << wheel));
                send_pointer(point.x, point.y);
            }
            return 0;
        }
        }
        return DefWindowProcW(m_window, message, wparam, lparam);
    }

    void register_window() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"librfb.d3d11.viewer";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(OCR_NORMAL));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error{"RegisterClassW failed"};
    }

    void create_window() {
        RECT rect{0, 0, m_client.get_fb_width(), m_client.get_fb_height()};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        m_window = CreateWindowExW(0, L"librfb.d3d11.viewer", L"LibRFB Viewer",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
            GetModuleHandleW(nullptr), this);
        if (!m_window) throw std::runtime_error{"CreateWindowExW failed"};
    }

    void create_d3d() {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = m_client.get_fb_width();
        desc.BufferDesc.Height = m_client.get_fb_height();
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = m_window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};
        auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &m_swapchain,
            &m_device, &level, &m_context);
        if (FAILED(result)) throw std::runtime_error{"D3D11 device creation failed"};
        m_width = m_client.get_fb_width();
        m_height = m_client.get_fb_height();
    }

    void resize_for_frame() {
        auto width = m_client.get_fb_width(), height = m_client.get_fb_height();
        if (width == m_width && height == m_height) return;
        m_context->ClearState();
        if (FAILED(m_swapchain->ResizeBuffers(0, width, height,
                                              DXGI_FORMAT_UNKNOWN, 0)))
            throw std::runtime_error{"D3D11 swap-chain resize failed"};
        m_width = width; m_height = height;
        RECT rect{0, 0, width, height};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(m_window, nullptr, 0, 0, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
    }

    void present() {
        resize_for_frame();
        m_pixels.resize(static_cast<std::size_t>(m_width) * m_height * 4);
        m_client.get_rfb(m_pixels);
        ComPtr<ID3D11Texture2D> backbuffer;
        if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
            throw std::runtime_error{"could not acquire D3D11 back buffer"};
        m_context->UpdateSubresource(backbuffer.Get(), 0, nullptr, m_pixels.data(),
                                     m_width * 4, 0);
        if (FAILED(m_swapchain->Present(1, 0)))
            throw std::runtime_error{"D3D11 present failed"};
    }

    void poll_rfb() {
        pollfd fd{m_client.get_socket(), POLLIN, 0};
        auto ready = socket_helper::poll_sockets(&fd, 1, 0);
        if (ready < 0) throw std::runtime_error{"RFB socket poll failed"};
        if (fd.revents & (POLLIN | POLLHUP)) {
            auto open = m_client.read_available_server_data();
            bool processed{};
            while (m_client.process_buffered_server_message()) {
                processed = true;
                if (m_client.is_frame_updated()) {
                    m_client.reset_frame_updated();
                    present();
                }
                if (!m_pending_remote_clipboard) {
                    std::string remote_clipboard;
                    if (m_client.take_server_cut_text(remote_clipboard))
                        m_pending_remote_clipboard = std::move(remote_clipboard);
                }
            }
            if (m_pending_remote_clipboard &&
                set_clipboard(*m_pending_remote_clipboard)) {
                m_pending_remote_clipboard.reset();
                std::string remote_clipboard;
                if (m_client.take_server_cut_text(remote_clipboard)) {
                    if (!set_clipboard(remote_clipboard))
                        m_pending_remote_clipboard = std::move(remote_clipboard);
                }
            }
            if (!open) {
                if (m_client.has_buffered_server_data())
                    throw std::runtime_error{"server closed with a truncated RFB message"};
                throw rfb::connection_closed{"server closed the connection"};
            }
            if (processed) m_client.request_framebuffer_update();
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw rfb::connection_closed{"server closed the connection"};
    }

    void send_pointer(int x, int y, std::optional<std::uint32_t> mask = {}) {
        RECT rect{}; GetClientRect(m_window, &rect);
        int cw = std::max(1L, rect.right), ch = std::max(1L, rect.bottom);
        x = std::clamp(x * m_client.get_fb_width() / cw, 0, m_client.get_fb_width() - 1);
        y = std::clamp(y * m_client.get_fb_height() / ch, 0, m_client.get_fb_height() - 1);
        m_client.send_pointer_event(mask.value_or(m_buttons), x, y);
    }
    void set_button(unsigned button, bool down, LPARAM lparam) {
        if (down) { SetCapture(m_window); m_buttons |= 1u << button; }
        else { m_buttons &= ~(1u << button); if (!m_buttons) ReleaseCapture(); }
        send_pointer(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
    }
    void release_input() {
        m_client.release_remote_input();
        m_physical_keysyms.clear();
        if (m_buttons) ReleaseCapture();
        m_buttons = 0;
    }

    bool set_clipboard(const std::string& text) {
        if (text.size() > static_cast<std::size_t>(INT_MAX)) return false;
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
        if (!chars && !text.empty()) return false;
        auto memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<std::size_t>(chars) + 1) * sizeof(wchar_t));
        if (!memory) return false;
        auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
        if (!wide) { GlobalFree(memory); return false; }
        if (chars) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), wide, chars);
        wide[chars] = 0;
        GlobalUnlock(memory);
        if (!OpenClipboard(m_window)) { GlobalFree(memory); return false; }
        bool success = false;
        if (EmptyClipboard()) {
            // Ownership transfers only when SetClipboardData succeeds.
            success = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!success) GlobalFree(memory);
        CloseClipboard();
        if (success) m_clipboard_sequence = GetClipboardSequenceNumber();
        return success;
    }
    void poll_clipboard() {
        auto sequence = GetClipboardSequenceNumber();
        if (sequence == m_clipboard_sequence || !sequence) return;
        if (m_client.get_view_only()) { m_clipboard_sequence = sequence; return; }
        if (!OpenClipboard(m_window)) return;
        auto handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            auto* wide = static_cast<const wchar_t*>(GlobalLock(handle));
            if (wide) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<std::size_t>(std::max(0, bytes)), '\0');
                if (bytes > 1) {
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                        utf8.data(), bytes, nullptr, nullptr);
                    utf8.resize(static_cast<std::size_t>(bytes - 1));
                }
                GlobalUnlock(handle);
                m_client.send_cut_text(utf8);
            }
        }
        CloseClipboard();
        m_clipboard_sequence = sequence;
    }

    config m_conf;
    client_type m_client;
    rfb_control::control_server<client_type> m_control;
    HWND m_window{};
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapchain;
    std::vector<std::uint8_t> m_pixels;
    std::optional<std::string> m_pending_remote_clipboard;
    std::map<std::uint32_t, std::uint32_t> m_physical_keysyms;
    int m_width{}, m_height{};
    std::uint32_t m_buttons{}, m_clipboard_sequence{};
    bool m_running{true};
};

} // namespace

int main(int argc, const char* argv[]) {
    try {
        if (argc < 3) throw std::logic_error{
            "usage: rfb_window_win32 <host> <port> [encodings] "
            "[--password text|--password-file path] [--view-only] "
            "[--control-port port]"};
        const char* host = argv[1];
        auto port = parse_port(argv[2], "port");
        std::vector<std::uint32_t> encodings;
        std::string password;
        bool view_only{};
        std::uint16_t control_port{};
        for (int i = 3; i < argc; ++i) {
            std::string_view arg{argv[i]};
            if (arg == "--password" && i + 1 < argc) password = argv[++i];
            else if (arg == "--password-file" && i + 1 < argc) password = read_password_file(argv[++i]);
            else if (arg == "--view-only") view_only = true;
            else if (arg == "--control-port" && i + 1 < argc)
                control_port = parse_port(argv[++i], "control port");
            else {
                auto parsed = rfb::parse_encodings(arg);
                if (!parsed) throw std::logic_error{"unknown option or encoding"};
                for (auto value : *parsed) encodings.push_back(
                    rfb::to_big_endian(static_cast<std::uint32_t>(value)));
            }
        }
        if (encodings.empty()) encodings = {rfb::to_big_endian(50u), rfb::to_big_endian(16u), 0};
        encodings.push_back(rfb::to_big_endian(
            static_cast<std::uint32_t>(rfb::encoding::scale_ext)));
        config conf{.address = host, .port = port,
                    .supported_encodings = std::move(encodings),
                    .view_only = view_only, .password = std::move(password),
                    .control_port = control_port};
        return win32_viewer{std::move(conf)}.run();
    } catch (const rfb::connection_closed& e) {
        std::cerr << e.what() << '\n'; return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}

#endif
