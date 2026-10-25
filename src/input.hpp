#pragma once

#include "terminal.hpp"

namespace cyberdeck {

struct Key {
  wint_t value = 0;
  bool special = false;
};

inline void forward_key(TerminalModel &model, Key key) {
  if (!key.special) {
    switch (key.value) {
      case 9: model.key(VTERM_KEY_TAB); return;
      case 10: case 13: model.key(VTERM_KEY_ENTER); return;
      case 27: model.key(VTERM_KEY_ESCAPE); return;
      case 127: case 8: model.key(VTERM_KEY_BACKSPACE); return;
      default: model.character(static_cast<uint32_t>(key.value)); return;
    }
  }
  VTermKey translated = VTERM_KEY_NONE;
  VTermModifier modifiers = VTERM_MOD_NONE;
  switch (key.value) {
    case KEY_UP: translated = VTERM_KEY_UP; break;
    case KEY_DOWN: translated = VTERM_KEY_DOWN; break;
    case KEY_LEFT: translated = VTERM_KEY_LEFT; break;
    case KEY_RIGHT: translated = VTERM_KEY_RIGHT; break;
    case KEY_HOME: translated = VTERM_KEY_HOME; break;
    case KEY_END: translated = VTERM_KEY_END; break;
    case KEY_IC: translated = VTERM_KEY_INS; break;
    case KEY_DC: translated = VTERM_KEY_DEL; break;
    case KEY_PPAGE: translated = VTERM_KEY_PAGEUP; break;
    case KEY_NPAGE: translated = VTERM_KEY_PAGEDOWN; break;
    case KEY_BACKSPACE: translated = VTERM_KEY_BACKSPACE; break;
    case KEY_ENTER: translated = VTERM_KEY_ENTER; break;
    case KEY_BTAB: translated = VTERM_KEY_TAB; modifiers = VTERM_MOD_SHIFT; break;
    default:
      if (key.value >= KEY_F(1) && key.value <= KEY_F(12))
        translated = static_cast<VTermKey>(VTERM_KEY_FUNCTION(key.value - KEY_F(0)));
      break;
  }
  if (translated != VTERM_KEY_NONE) model.key(translated, modifiers);
}

}  // namespace cyberdeck
