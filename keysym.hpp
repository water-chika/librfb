#pragma once

// X11 keysyms, which is what RFB KeyEvent carries, translated to what the
// local platform wants. Only the Windows side needs virtual key codes; the
// Unicode half is portable.

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rfb::server {

// Returns the Unicode code point a keysym stands for, or 0 for keysyms that
// are not characters (function keys, modifiers, ...). Latin-1 keysyms are
// their own code point, and everything else is 0x01000000 plus the code
// point, per the X11 keysym encoding RFB inherits.
inline uint32_t keysym_to_unicode(uint32_t keysym) {
    if (keysym >= 0x20 && keysym <= 0x7e) {
        return keysym;
    }
    if (keysym >= 0xa0 && keysym <= 0xff) {
        return keysym;
    }
    if ((keysym & 0xff000000u) == 0x01000000u) {
        return keysym & 0x00ffffffu;
    }
    return 0;
}

#ifdef _WIN32

// Non-character keys only. Characters go through VkKeyScan or Unicode
// injection instead, so that the client's keyboard layout does not have to
// match the server's.
inline WORD keysym_to_virtual_key(uint32_t keysym) {
    switch (keysym) {
    case 0xff08: return VK_BACK;
    case 0xff09: return VK_TAB;
    case 0xff0b: return VK_CLEAR;
    case 0xff0d: return VK_RETURN;
    case 0xff13: return VK_PAUSE;
    case 0xff14: return VK_SCROLL;
    case 0xff1b: return VK_ESCAPE;
    case 0xff50: return VK_HOME;
    case 0xff51: return VK_LEFT;
    case 0xff52: return VK_UP;
    case 0xff53: return VK_RIGHT;
    case 0xff54: return VK_DOWN;
    case 0xff55: return VK_PRIOR;
    case 0xff56: return VK_NEXT;
    case 0xff57: return VK_END;
    case 0xff61: return VK_SNAPSHOT;
    case 0xff63: return VK_INSERT;
    case 0xff67: return VK_APPS;
    case 0xffff: return VK_DELETE;

    // Keypad.
    case 0xff8d: return VK_RETURN;
    case 0xff95: return VK_HOME;
    case 0xff96: return VK_LEFT;
    case 0xff97: return VK_UP;
    case 0xff98: return VK_RIGHT;
    case 0xff99: return VK_DOWN;
    case 0xff9a: return VK_PRIOR;
    case 0xff9b: return VK_NEXT;
    case 0xff9c: return VK_END;
    case 0xff9e: return VK_INSERT;
    case 0xff9f: return VK_DELETE;
    case 0xffaa: return VK_MULTIPLY;
    case 0xffab: return VK_ADD;
    case 0xffad: return VK_SUBTRACT;
    case 0xffae: return VK_DECIMAL;
    case 0xffaf: return VK_DIVIDE;
    case 0xffb0: case 0xffb1: case 0xffb2: case 0xffb3: case 0xffb4:
    case 0xffb5: case 0xffb6: case 0xffb7: case 0xffb8: case 0xffb9:
        return static_cast<WORD>(VK_NUMPAD0 + (keysym - 0xffb0));

    // Function keys. F13 and above have no useful Windows equivalent.
    case 0xffbe: case 0xffbf: case 0xffc0: case 0xffc1: case 0xffc2:
    case 0xffc3: case 0xffc4: case 0xffc5: case 0xffc6: case 0xffc7:
    case 0xffc8: case 0xffc9:
        return static_cast<WORD>(VK_F1 + (keysym - 0xffbe));

    // Modifiers. Meta and Super both land on the Windows key, which is what
    // viewers expect from a Windows server.
    case 0xffe1: return VK_LSHIFT;
    case 0xffe2: return VK_RSHIFT;
    case 0xffe3: return VK_LCONTROL;
    case 0xffe4: return VK_RCONTROL;
    case 0xffe5: return VK_CAPITAL;
    case 0xff7f: return VK_NUMLOCK;
    case 0xffe7: case 0xffeb: return VK_LWIN;
    case 0xffe8: case 0xffec: return VK_RWIN;
    case 0xffe9: return VK_LMENU;
    case 0xffea: return VK_RMENU;
    default: return 0;
    }
}

inline bool is_control_keysym(uint32_t keysym) {
    return keysym == 0xffe3 || keysym == 0xffe4;
}
inline bool is_alt_keysym(uint32_t keysym) {
    return keysym == 0xffe9 || keysym == 0xffea;
}

#endif // _WIN32

} // namespace rfb::server
