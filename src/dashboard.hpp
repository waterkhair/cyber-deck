#pragma once

#include "audio.hpp"
#include "bluetooth.hpp"
#include "cookies.hpp"
#include "menu_art.hpp"
#include "system.hpp"
#include "terminal.hpp"
#include "theme.hpp"
#include "wifi.hpp"
#include "wave.hpp"
#include "web.hpp"

#include <ctime>
#include <cstring>
#include <utility>

namespace cyberdeck {

constexpr std::array<const char *, 6> kTabs = {
    "Dashboard", "Terminal", "Media", "Notes", "Web", "Settings"};
constexpr std::array<const char *, 5> kSettingsSections = {
    "Theme", "Network", "Audio", "Bluetooth", "Web"};
enum class HelpDialog { None, Confirm, Password, TextEntry };

struct ChromeView {
  size_t tab = 0;
  size_t menu_selection = 0;
  bool menu_focused = true;
  HelpDialog help_dialog = HelpDialog::None;
  Theme theme = Theme::Husky;
  size_t settings_selection = 0;
  size_t theme_selection = 0;
  CookiePolicy cookie_policy = CookiePolicy::Block;
  size_t cookie_selection = 0;
  bool show_web_header_links = false;
  bool wifi_page = false;
  bool audio_page = false;
  bool bluetooth_page = false;
  size_t wifi_selection = 0;
  WifiSnapshot wifi;
  size_t audio_selection = 0;
  size_t audio_picker_selection = 0;
  bool audio_picker = false;
  AudioSnapshot audio;
  size_t bluetooth_selection = 0;
  BluetoothSnapshot bluetooth;
  size_t notes_section = 0;
  size_t web_tab = 0;
  std::array<bool, 4> web_tabs{};
  size_t wave_selection = 0;
  WaveSnapshot wave;
  std::vector<WaveStation> wave_stations;
  std::vector<std::string> notes;
  size_t note_selection = 0;
  bool note_editing = false;
  bool web_browsing = false;
  SystemOverview overview;
  std::array<bool, kMaxSessions> terminals{};
  size_t active_terminal = 0;
};

struct ModalView {
  enum class Kind { None, Confirm, Password, Info, AudioPicker, TextEntry } kind = Kind::None;
  std::string message;
  std::string detail;
  std::string accept_label;
  bool accept_selected = false;
};

class Renderer {
 public:
  explicit Renderer(bool default_colors) : default_colors_(default_colors) {
    ghostty_ = layout_.inset == 0;
    set_theme(Theme::Husky);
  }

  ~Renderer() { restore_host_background(); }

  void set_theme(Theme theme) {
    theme_ = theme;
    pairs_.clear();
    next_pair_ = 5;
    chrome_colors_ = false;
    set_host_background(theme);
    if (!has_colors() || COLORS < 8 || COLOR_PAIRS <= 4) return;
    if (theme == Theme::Greenline) {
      // Match cyber-ware's mint text and dark green selection where 256 colors exist.
      const bool extended = COLORS >= 256;
      const short background = extended ? 232 : COLOR_BLACK;
      const short primary = extended ? 194 : (COLORS >= 16 ? 10 : COLOR_GREEN);
      const short accent = extended ? 158 : COLOR_GREEN;
      chrome_colors_ = init_pair(1, primary, background) == OK &&
                       init_pair(2, accent, background) == OK &&
                       init_pair(3, extended ? primary : COLOR_BLACK,
                                 extended ? 22 : COLOR_GREEN) == OK &&
                       init_pair(4, extended ? accent : COLOR_BLACK,
                                 extended ? 22 : COLOR_GREEN) == OK;
      return;
    }
    const short background = theme == Theme::Synthwave && COLORS >= 256 ? 17 :
                             default_colors_ && theme == Theme::Husky ? -1 : COLOR_BLACK;
    const short primary = theme == Theme::Synthwave ? (COLORS >= 256 ? 225 : COLOR_MAGENTA) : COLOR_WHITE;
    const short accent = theme == Theme::Synthwave ? (COLORS >= 256 ? 45 : COLOR_CYAN) : COLOR_WHITE;
    const short selection = theme == Theme::Synthwave ? (COLORS >= 256 ? 201 : COLOR_MAGENTA) : COLOR_WHITE;
    chrome_colors_ = init_pair(1, primary, background) == OK &&
                     init_pair(2, accent, background) == OK &&
                     init_pair(3, COLOR_BLACK, selection) == OK &&
                     init_pair(4, COLOR_BLACK, theme == Theme::Synthwave ? accent : selection) == OK;
  }

  void draw(TerminalModel *model, const ChromeView &view,
            const std::string &status, const ModalView &modal) {
    int host_rows = 0, host_cols = 0;
    getmaxyx(stdscr, host_rows, host_cols);
    if (modal.kind != ModalView::Kind::None) {
      (void)wbkgdset(stdscr, static_cast<chtype>(' ') | body_style());
      erase();
      if (modal.kind == ModalView::Kind::AudioPicker)
        draw_audio_picker(view, host_rows, host_cols);
      else if (modal.kind == ModalView::Kind::Info)
        draw_help(view, host_rows, host_cols);
      else
        draw_modal(modal, host_rows, host_cols);
      if (modal.kind != ModalView::Kind::Info)
        draw_help_hint(layout_.inset, layout_.inset,
                       std::max(0, host_cols - 2 * layout_.inset), 0);
      (void)curs_set(0);
      (void)wnoutrefresh(stdscr);
      (void)doupdate();
      if (model) model->clear_dirty();
      return;
    }
    // The inset is themed space inside the app, including around a PTY pane.
    (void)wbkgdset(stdscr, static_cast<chtype>(' ') | body_style());
    erase();
    const int pane_rows = std::max(0, host_rows - layout_.reserved_rows());
    const int pane_cols = std::max(0, host_cols - 2 * layout_.inset);
    if (view.menu_focused) {
      draw_preview(view, pane_rows, layout_.inset, layout_.terminal_top(), pane_cols);
      draw_help_hint(layout_.inset, layout_.inset, pane_cols, 0);
    } else {
      if (host_rows > layout_.inset)
        draw_content_menu(view, layout_.inset, layout_.inset, pane_cols);
      if (!model) draw_page(view, pane_rows, layout_.inset, layout_.terminal_top(), pane_cols);
    }
    const int rows = model ? std::min(pane_rows, model->rows()) : 0;
    const int cols = model ? std::min(pane_cols, model->cols()) : 0;
    for (int row = 0; row < rows; ++row) {
      for (int col = 0; col < cols; ++col) {
        VTermScreenCell cell{};
        if (!vterm_screen_get_cell(model->screen(), {row, col}, &cell) || cell.width == 0) continue;
        wchar_t characters[VTERM_MAX_CHARS_PER_CELL + 1]{};
        int character_count = 0;
        for (uint32_t codepoint : cell.chars) {
          if (!codepoint) break;
          if (codepoint > static_cast<uint32_t>(WCHAR_MAX) ||
              (codepoint >= 0xD800 && codepoint <= 0xDFFF)) continue;
          characters[character_count++] = static_cast<wchar_t>(codepoint);
        }
        if (!character_count) characters[0] = L' ';
        attr_t attributes = A_NORMAL;
        if (cell.attrs.bold) attributes |= A_BOLD;
        if (cell.attrs.underline) attributes |= A_UNDERLINE;
        if (cell.attrs.reverse) attributes |= A_REVERSE;
        if (cell.attrs.italic) attributes |= A_ITALIC;
        if (cell.attrs.blink) attributes |= A_BLINK;
        if (cell.attrs.conceal) attributes |= A_INVIS;
        cchar_t glyph{};
        if (setcchar(&glyph, characters, attributes, color_pair(cell.fg, cell.bg), nullptr) == OK)
          (void)mvadd_wch(row + layout_.terminal_top(), col + layout_.inset, &glyph);
      }
    }

    if (host_rows > 2 * layout_.inset && pane_cols > 0)
      draw_footer(host_rows - layout_.inset - 1, layout_.inset, pane_cols, status);
    VTermPos cursor{};
    if (model) vterm_state_get_cursorpos(model->state(), &cursor);
    if (model && !view.menu_focused && model->cursor_visible() && cursor.row >= 0 && cursor.row < rows &&
        cursor.col >= 0 && cursor.col < cols) {
      (void)curs_set(1);
      (void)move(cursor.row + layout_.terminal_top(), cursor.col + layout_.inset);
    } else {
      (void)curs_set(0);
    }
    (void)wnoutrefresh(stdscr);
    (void)doupdate();
    if (model) model->clear_dirty();
  }

 private:
  static void write_terminal_sequence(const char *sequence) {
    (void)std::fflush(stdout);
    const size_t length = std::strlen(sequence);
    size_t sent = 0;
    while (sent < length) {
      const ssize_t count = ::write(STDOUT_FILENO, sequence + sent, length - sent);
      if (count > 0) sent += static_cast<size_t>(count);
      else if (count < 0 && errno == EINTR) continue;
      else break;
    }
  }

  void restore_host_background() {
    if (!ghostty_ || !host_background_changed_) return;
    write_terminal_sequence("\x1b]111\x1b\\");
    host_background_changed_ = false;
  }

  void set_host_background(Theme theme) {
    if (!ghostty_) return;
    if (theme == Theme::Husky) {
      restore_host_background();
      return;
    }
    if (theme == Theme::Synthwave)
      write_terminal_sequence(COLORS >= 256 ? "\x1b]11;#00005f\x1b\\" :
                                               "\x1b]11;#000000\x1b\\");
    else
      write_terminal_sequence(COLORS >= 256 ? "\x1b]11;#080808\x1b\\" :
                                               "\x1b]11;#000000\x1b\\");
    host_background_changed_ = true;
  }

  attr_t title_style() const {
    return A_BOLD | (chrome_colors_ ? COLOR_PAIR(1) : 0);
  }
  attr_t section_style() const {
    return A_BOLD | (chrome_colors_ ? COLOR_PAIR(2) : 0);
  }
  attr_t body_style() const { return chrome_colors_ ? COLOR_PAIR(1) : A_NORMAL; }
  attr_t muted_style() const { return A_DIM | body_style(); }
  attr_t selected_style() const {
    return A_BOLD | (chrome_colors_ ? COLOR_PAIR(3) : A_REVERSE);
  }
  attr_t alert_style() const {
    return A_BOLD | (chrome_colors_ ? COLOR_PAIR(4) : A_REVERSE);
  }
  attr_t modal_style() const { return body_style(); }
  attr_t modal_selected_style() const { return selected_style(); }

  void draw_centered(int row, int cols, const std::string &value, attr_t style,
                     int max_cells = 0) {
    int rows = 0, ignored_cols = 0;
    getmaxyx(stdscr, rows, ignored_cols);
    if (row < 0 || row >= rows || cols <= 0) return;
    std::wstring glyphs;
    int cells = 0;
    std::mbstate_t state{};
    const char *source = value.data();
    size_t remaining = value.size();
    while (remaining) {
      wchar_t glyph = L'?';
      size_t bytes = std::mbrtowc(&glyph, source, remaining, &state);
      if (bytes == static_cast<size_t>(-1) || bytes == static_cast<size_t>(-2)) {
        state = {};
        glyph = L'?';
        bytes = 1;
      } else if (bytes == 0) break;
      int width = wcwidth(glyph);
      if (width < 0) { glyph = L'?'; width = 1; }
      if (cells + width > (max_cells > 0 ? std::min(cols, max_cells) : cols)) break;
      glyphs.push_back(glyph);
      cells += width;
      source += bytes;
      remaining -= bytes;
    }
    if (glyphs.empty()) return;
    attron(style);
    (void)mvaddnwstr(row, (cols - cells) / 2, glyphs.c_str(), static_cast<int>(glyphs.size()));
    attroff(style);
  }

  void draw_help(const ChromeView &view, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    using Shortcut = std::pair<std::string, std::string>;
    const std::vector<Shortcut> global = {
        {"Alt+d", "Dashboard"}, {"Alt+t", "Terminal"},
        {"Alt+m", "Media"},
        {"Alt+w", "Web"}, {"Alt+s", "Settings"},
        {"Alt+q", "Quit"}, {"Alt+Esc", "Back / cancel"},
        {"Shift+Alt+?", "Toggle Help"}};
    std::string section;
    std::vector<Shortcut> local;
    std::string note;
    if (view.help_dialog == HelpDialog::Confirm) {
      section = "Confirmation";
      local = {{"h", "Select Yes"}, {"l", "Select No"},
               {"Left", "Select Yes"}, {"Right", "Select No"},
               {"Tab", "Change choice"}, {"Enter", "Use choice"},
               {"Alt+y", "Yes"}, {"Alt+n", "No"}};
    } else if (view.help_dialog == HelpDialog::Password) {
      section = "Password prompt";
      local = {{"Enter", "Submit"}, {"Backspace", "Delete"},
               {"Left", "Choose action"}, {"Right", "Choose action"}};
    } else if (view.help_dialog == HelpDialog::TextEntry) {
      section = "Text prompt";
      local = {{"Enter", "Continue / save"}, {"Backspace", "Delete"},
               {"Ctrl+u", "Clear entry"}};
    } else if (view.menu_focused) {
      section = std::string(kTabs[view.tab]) + " preview";
      if (view.tab > 0) local.emplace_back("Alt+h", "Previous module");
      if (view.tab + 1 < kTabs.size()) local.emplace_back("Alt+l", "Next module");
      local.emplace_back("Enter", "Open module");
    } else if (view.tab == 0) {
      section = "Dashboard";
    } else if (view.tab == 1) {
      section = "Terminal";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next / new tab"},
               {"Alt+1", "Terminal 1"}, {"Alt+2", "Terminal 2"},
               {"Alt+3", "Terminal 3"}, {"Alt+4", "Terminal 4"},
               {"Alt+x", "Close tab if 2+"},
               {"Ctrl+c", "Interrupt"}, {"Ctrl+z", "Suspend"},
               {"Ctrl+d", "Exit shell"}};
    } else if (view.tab == 2) {
      section = "Media / Wave";
      local = {{"j", "Next station"}, {"k", "Previous station"},
               {"Enter", "Play / stop"}, {"a", "Add station"},
               {"s", "Stop playback"}};
    } else if (view.tab == 3 && view.notes_section == 0) {
      section = "Notes / Text";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next tab"},
               {"n", "New note"}, {"j", "Next note"},
               {"k", "Previous note"}, {"Enter", "Edit note"},
               {"Ctrl+o", "Save in nano"}, {"Ctrl+x", "Exit nano"}};
    } else if (view.tab == 3) {
      section = "Notes / Voice";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next tab"}};
      note = "Recording is not available yet";
    } else if (view.tab == 4) {
      section = "Web";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next / new tab"},
               {"Alt+x", "Close tab if 2+"}};
      if (view.web_browsing) {
        local.emplace_back("h/j/k/l", "Back / links / open");
        local.emplace_back("g", "Open URL in ELinks");
        local.emplace_back("q", "Exit ELinks page");
      } else {
        local.emplace_back("w", "Wikipedia search");
        local.emplace_back("b", "Browse URL");
        local.emplace_back("d", "DuckDuckGo search");
        local.emplace_back("g", "Google search");
      }
    } else if (view.tab == 5 && view.settings_selection == 0) {
      section = "Settings / Theme";
      local = {{"Alt+h", "Previous section"}, {"Alt+l", "Next section"},
               {"j", "Next theme"}, {"k", "Previous theme"},
               {"Enter", "Apply theme"}};
    } else if (view.tab == 5 && view.settings_selection == 1) {
      section = "Settings / Network";
      local = {{"Alt+h", "Previous section"}, {"Alt+l", "Next section"},
               {"j", "Next item"}, {"k", "Previous item"},
               {"Enter", "Connect selected"}, {"r", "Refresh / scan WiFi"},
               {"d", "Disconnect selected"}, {"w", "WiFi on/off"},
               {"p", "WiFi password"}, {"f", "Forget WiFi"}};
    } else if (view.tab == 5 && view.audio_picker) {
      section = "Settings / Audio picker";
      local = {{"j", "Next device"}, {"k", "Previous device"},
               {"Enter", "Use device"}};
    } else if (view.tab == 5 && view.settings_selection == 2) {
      section = "Settings / Audio";
      local = {{"Alt+h", "Previous section"}, {"Alt+l", "Next section"},
               {"j", "Select input"}, {"k", "Select output"},
               {"Enter", "Choose device"}, {"Left", "Volume down"},
               {"Right", "Volume up"}, {"r", "Refresh"}};
    } else if (view.tab == 5 && view.settings_selection == 4) {
      section = "Settings / Web";
      local = {{"Alt+h", "Previous section"}, {"Alt+l", "Next section"},
               {"j", "Next option"}, {"k", "Previous option"},
               {"Enter", "Apply / toggle option"}};
      note = "ELinks cannot identify necessary cookies";
    } else if (view.tab == 5) {
      section = "Settings / Bluetooth";
      local = {{"Alt+h", "Previous section"}, {"Alt+l", "Next section"},
               {"j", "Next device"}, {"k", "Previous device"},
               {"Enter", "Connect / pair"}, {"r", "Scan"},
               {"w", "Power on/off"}, {"p", "Pair"},
               {"d", "Disconnect"}, {"t", "Trust / untrust"},
               {"f", "Forget device"}};
    }
    if (view.help_dialog == HelpDialog::None) {
      if (!view.audio_picker && !view.menu_focused) {
        if (view.tab == 1) local.emplace_back("Esc", "Send to shell");
        else if (view.tab == 3 && view.note_editing)
          local.emplace_back("Esc", "Send to nano");
        else if (view.tab == 4 && view.web_browsing)
          local.emplace_back("Esc", "ELinks menu");
      }
      local.emplace_back("Alt+n", "Notes");
    }

    const int columns = cols >= 84 ? 3 : cols >= 48 ? 2 : 1;
    const int gap = 2;
    const int block_width = std::max(1, cols - 4);
    const int cell_width = std::max(1, (block_width - gap * (columns - 1)) / columns);
    const int grid_width = columns * cell_width + gap * (columns - 1);
    const int left = std::max(0, (cols - grid_width) / 2);
    auto grid_rows = [columns](size_t count) {
      return static_cast<int>((count + static_cast<size_t>(columns) - 1) / columns);
    };
    const int base_height = 4 + grid_rows(global.size()) + grid_rows(local.size()) +
                            (note.empty() ? 0 : 1);
    const int section_gap = rows >= base_height + 2 ? 1 : 0;
    int row = std::max(0, (rows - base_height - 2 * section_gap) / 2);
    draw_centered(row++, cols, "HELP", title_style());
    auto draw_section = [&](const std::string &heading, const std::vector<Shortcut> &items) {
      draw_text(row++, left, grid_width, "[" + heading + "]", section_style());
      for (size_t i = 0; i < items.size(); ++i) {
        const int x = left + static_cast<int>(i % columns) * (cell_width + gap);
        const int y = row + static_cast<int>(i / columns);
        if (y >= rows) break;
        const std::string key = items[i].first + ": ";
        draw_text(y, x, cell_width, key, section_style());
        if (static_cast<int>(key.size()) < cell_width)
          draw_text(y, x + static_cast<int>(key.size()),
                    cell_width - static_cast<int>(key.size()), items[i].second, body_style());
      }
      row += grid_rows(items.size());
    };
    row += section_gap;
    draw_section("GLOBAL", global);
    row += section_gap;
    draw_section(section, local);
    if (!note.empty() && row < rows)
      draw_text(row++, left, grid_width, note, muted_style());
    if (row < rows) draw_centered(row, cols, "Alt+Esc closes Help", muted_style());
  }

  void draw_modal(const ModalView &modal, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    const int center = rows / 2;
    if (modal.kind == ModalView::Kind::TextEntry) {
      draw_centered(center - 2, cols, modal.message, section_style());
      draw_centered(center, cols, modal.detail, modal_style(), std::max(1, cols - 4));
      draw_centered(center + 2, cols, modal.accept_label, muted_style());
      return;
    }
    const bool password = modal.kind == ModalView::Kind::Password;
    draw_centered(center - (password ? 2 : 1), cols, modal.message,
                  section_style());
    if (!modal.detail.empty())
      draw_centered(center, cols, modal.detail, modal_style());
    const int button_row = center + (password || !modal.detail.empty() ? 2 : 0);
    const std::string accept = "[" + modal.accept_label + "]";
    const std::string cancel = modal.kind == ModalView::Kind::Password ? "[Cancel]" : "[No]";
    const int width = static_cast<int>(accept.size() + cancel.size() + 1);
    if (width > cols) {
      const std::string &button = modal.accept_selected ? accept : cancel;
      draw_centered(button_row, cols, button, modal_selected_style());
      if (modal.kind == ModalView::Kind::Confirm && static_cast<int>(button.size()) <= cols)
        draw_text(button_row, (cols - static_cast<int>(button.size())) / 2 + 1, 1,
                  button.substr(1, 1), modal_selected_style() | A_UNDERLINE);
      return;
    }
    const int left = (cols - width) / 2;
    const attr_t accept_style = modal.accept_selected ? modal_selected_style() : modal_style();
    const attr_t cancel_style = modal.accept_selected ? modal_style() : modal_selected_style();
    draw_text(button_row, left, static_cast<int>(accept.size()), accept, accept_style);
    draw_text(button_row, left + static_cast<int>(accept.size()) + 1,
              static_cast<int>(cancel.size()), cancel, cancel_style);
    if (modal.kind == ModalView::Kind::Confirm) {
      draw_text(button_row, left + 1, 1, "Y", accept_style | A_UNDERLINE);
      draw_text(button_row, left + static_cast<int>(accept.size()) + 2, 1,
                "N", cancel_style | A_UNDERLINE);
    }
  }

  void draw_audio_picker(const ChromeView &view, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    const bool output = view.audio_selection == 0;
    const auto &devices = output ? view.audio.outputs : view.audio.inputs;
    const int visible = std::min<int>(static_cast<int>(devices.size()),
                                      std::min(10, std::max(1, rows - 6)));
    const int top = std::max(0, (rows - visible - 4) / 2);
    const int width = std::max(1, cols - 4);
    draw_centered(top, cols, output ? "Select output" : "Select input", section_style(), width);
    if (devices.empty()) {
      draw_centered(top + 2, cols,
                    output ? "No outputs available" : "No inputs available", modal_style(), width);
      draw_centered(top + 3, cols, "Alt+Esc back", muted_style(), width);
      return;
    }
    const size_t selected = std::min(view.audio_picker_selection, devices.size() - 1);
    const size_t start = selected >= static_cast<size_t>(visible) ?
        selected - static_cast<size_t>(visible) + 1 : 0;
    for (int row = 0; row < visible; ++row) {
      const size_t index = start + static_cast<size_t>(row);
      if (index >= devices.size()) break;
      const AudioDevice &device = devices[index];
      draw_centered(top + 2 + row, cols,
                    std::string(device.is_default ? "* " : "  ") + device.description,
                    index == selected ? modal_selected_style() : modal_style(), width);
    }
  }

  void draw_text(int row, int col, int width, const std::string &value,
                 attr_t style, bool fill = false) {
    if (width <= 0) return;
    attron(style);
    if (fill) for (int x = 0; x < width; ++x) (void)mvaddch(row, col + x, ' ');
    (void)mvaddnstr(row, col, value.c_str(), width);
    attroff(style);
  }

  void draw_preview(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (pane_rows <= 0 || cols <= 0) return;
    const int center_row = top + pane_rows / 2 - (pane_rows >= 6 ? 1 : 0);
    if (view.tab == 0) {
      draw_page(view, pane_rows, left, top, cols);
      return;
    }
    const auto &art = kMenuArt[view.tab - 1];
    size_t width = 0;
    for (const char *line : art) width = std::max(width, std::strlen(line));
    if (pane_rows >= 5 && static_cast<int>(width) <= cols - 2) {
      const int start = left + (cols - static_cast<int>(width)) / 2;
      for (int row = 0; row < 5; ++row)
        draw_text(center_row - 2 + row, start, static_cast<int>(width), art[row], title_style());
    } else {
      const std::string label = "[" + std::string(kTabs[view.tab]) + "]";
      const int offset = std::max(0, (cols - static_cast<int>(label.size())) / 2);
      draw_text(center_row, left + offset, cols - offset, label, title_style());
    }
  }

  void draw_content_menu(const ChromeView &view, int row, int left, int cols) {
    int occupied = 0;
    if (view.tab == 1) {
      int col = 0;
      for (size_t i = 0; i < kMaxSessions; ++i) {
        if (!view.terminals[i]) continue;
        const std::string label = "[" + std::to_string(i + 1) + "]";
        if (col + static_cast<int>(label.size()) > cols) break;
        draw_text(row, left + col, static_cast<int>(label.size()), label,
                  i == view.active_terminal ? selected_style() : body_style());
        col += static_cast<int>(label.size()) + 1;
      }
      if (col == 0) draw_text(row, left, cols, "[Terminal]", section_style());
      occupied = col == 0 ? 10 : col - 1;
    } else if (view.tab == 2) {
      draw_text(row, left, cols, "[Wave]", selected_style());
      occupied = 6;
    } else if (view.tab == 3) {
      draw_text(row, left, cols, "[Text] [Voice]", body_style());
      draw_text(row, left + (view.notes_section == 0 ? 0 : 7),
                view.notes_section == 0 ? 6 : 7,
                view.notes_section == 0 ? "[Text]" : "[Voice]", selected_style());
      occupied = 14;
    } else if (view.tab == 4) {
      int col = 0;
      for (size_t i = 0; i < view.web_tabs.size(); ++i) {
        if (!view.web_tabs[i]) continue;
        const std::string label = "[" + std::to_string(i + 1) + "]";
        if (col + static_cast<int>(label.size()) > cols) break;
        draw_text(row, left + col, static_cast<int>(label.size()), label,
                  i == view.web_tab ? selected_style() : body_style());
        col += static_cast<int>(label.size()) + 1;
      }
      occupied = std::max(0, col - 1);
    } else if (view.tab == 5) {
      int col = 0;
      for (size_t i = 0; i < kSettingsSections.size(); ++i) {
        const std::string label = "[" + std::string(kSettingsSections[i]) + "]";
        if (col + static_cast<int>(label.size()) > cols) break;
        draw_text(row, left + col, static_cast<int>(label.size()), label,
                  i == view.settings_selection ? selected_style() : body_style());
        col += static_cast<int>(label.size()) + 1;
      }
      occupied = std::max(0, col - 1);
    } else {
      draw_text(row, left, cols, "[" + std::string(kTabs[view.tab]) + "]", section_style());
    }
    draw_help_hint(row, left, cols, occupied);
  }

  void draw_help_hint(int row, int left, int cols, int occupied) {
    const std::string hint = "Shift+Alt+? Help";
    if (occupied + 2 + static_cast<int>(hint.size()) <= cols)
      draw_text(row, left + cols - static_cast<int>(hint.size()),
                static_cast<int>(hint.size()), hint, section_style());
  }

  void draw_page(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (pane_rows <= 0 || cols <= 0) return;
    const size_t tab = view.tab;
    if (tab == 5) {
      draw_settings(view, pane_rows, left, top, cols);
      return;
    }
    if (tab == 2) {
      draw_wave(view, pane_rows, left, top, cols);
      return;
    }
    if (tab == 3) {
      draw_notes(view, pane_rows, left, top, cols);
      return;
    }
    if (tab == 4) {
      draw_web(view, pane_rows, left, top, cols);
      return;
    }
    const std::string heading = tab == 0 ? "SYSTEM OVERVIEW" :
                                tab == 1 ? "TERMINAL" :
                                std::string(kTabs[tab]);
    draw_text(top, left, cols, heading, title_style());
    if (pane_rows > 1) {
      attron(muted_style());
      (void)mvhline(top + 1, left, '-', cols);
      attroff(muted_style());
    }
    if (tab == 0) {
      // The overview values are supplied by the application at a low refresh rate.
      const std::array<std::pair<const char *, const std::string *>, 5> rows = {{
          {"LOAD", &view.overview.load}, {"MEMORY", &view.overview.memory},
          {"UPTIME", &view.overview.uptime}, {"NETWORK", &view.overview.network},
          {"BATTERY", &view.overview.battery}}};
      std::time_t now = std::time(nullptr);
      std::tm local{};
      char clock[6] = "--:--";
      if (localtime_r(&now, &local)) (void)std::strftime(clock, sizeof(clock), "%H:%M", &local);
      if (pane_rows > 3) {
        draw_text(top + 3, left, std::min(cols, 10), "TIME", section_style());
        if (cols > 10) draw_text(top + 3, left + 10, cols - 10, clock, body_style());
      }
      for (size_t i = 0; i < rows.size(); ++i) {
        const int row = top + 4 + static_cast<int>(i);
        if (row >= top + pane_rows) break;
        draw_text(row, left, std::min(cols, 10), rows[i].first, section_style());
        if (cols > 10)
          draw_text(row, left + 10, cols - 10, *rows[i].second, body_style());
      }
    } else if (tab == 1) {
      if (pane_rows > 3) draw_text(top + 3, left, cols,
          "No terminal. Alt+l creates one.", muted_style());
    } else if (pane_rows > 3) {
      draw_text(top + 3, left, cols, "PLANNED", section_style());
    }
  }

  void draw_field(int row, int left, int cols, const char *label,
                  const std::string &value, attr_t value_style) {
    const std::string tag = "[" + std::string(label) + "] ";
    draw_text(row, left, cols, tag, section_style());
    const int offset = static_cast<int>(tag.size());
    if (cols > offset)
      draw_text(row, left + offset, cols - offset, value, value_style);
  }

  void draw_notes(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (view.notes_section == 1) {
      draw_text(top, left, cols, "Voice notes", section_style());
      if (pane_rows > 2)
        draw_text(top + 2, left, cols, "Audio recording is not available yet.", muted_style());
      return;
    }
    if (view.note_editing) return;
    if (pane_rows < 1) return;
    if (view.notes.empty())
      draw_text(top, left, cols, "No notes yet. Press n to create one.", muted_style());
    const int available = std::max(0, pane_rows);
    const size_t start = available > 0 && view.note_selection >= static_cast<size_t>(available) ?
        view.note_selection - static_cast<size_t>(available) + 1 : 0;
    for (int row = 0; row < available && start + static_cast<size_t>(row) < view.notes.size(); ++row) {
      const size_t index = start + static_cast<size_t>(row);
      draw_text(top + row, left, cols, "[" + view.notes[index] + "]",
                index == view.note_selection ? selected_style() : body_style());
    }
  }

  void draw_web(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (view.web_browsing || pane_rows < 1) return;
    draw_text(top, left, cols, "[w] Wikipedia search", body_style());
    if (pane_rows > 1) draw_text(top + 1, left, cols, "[b] Browse URL", body_style());
    if (pane_rows > 2) draw_text(top + 2, left, cols, "[d] DuckDuckGo search", body_style());
    if (pane_rows > 3) draw_text(top + 3, left, cols, "[g] Google search", body_style());
  }

  void draw_settings(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (pane_rows <= 0) return;
    if (view.settings_selection == 1) {
      draw_network(view, pane_rows, left, top, cols);
      return;
    }
    if (view.settings_selection == 2) {
      draw_audio(view, pane_rows, left, top, cols);
      return;
    }
    if (view.settings_selection == 3) {
      draw_bluetooth(view, pane_rows, left, top, cols);
      return;
    }
    if (view.settings_selection == 4) {
      if (pane_rows > 0) draw_text(top, left, cols, "Cookies", section_style());
      for (size_t i = 0; i < kCookiePolicyNames.size(); ++i) {
        const int row = top + 2 + static_cast<int>(i);
        if (row >= top + pane_rows) break;
        const std::string label = "[" + std::string(kCookiePolicyNames[i]) + "]" +
            (view.cookie_policy == static_cast<CookiePolicy>(i) ? " *" : "");
        draw_text(row, left, cols, label,
                  view.cookie_selection == i ? selected_style() : body_style());
      }
      const int links_row = top + 2 + static_cast<int>(kCookiePolicyNames.size());
      if (links_row < top + pane_rows) {
        const std::string label = "Page header links: " +
            std::string(view.show_web_header_links ? "On" : "Off");
        draw_text(links_row, left, cols, label,
                  view.cookie_selection == kCookiePolicyNames.size() ? selected_style() : body_style());
      }
      if (links_row + 2 < top + pane_rows)
        draw_text(links_row + 2, left, cols,
                  "ELinks cannot identify necessary cookies.", muted_style());
      if (links_row + 3 < top + pane_rows)
        draw_text(links_row + 3, left, cols,
                  "Block all may break some websites.", muted_style());
      return;
    }
    for (size_t i = 0; i < kThemeNames.size(); ++i) {
      const int row = top + static_cast<int>(i);
      if (row >= top + pane_rows) break;
      const std::string label = "[" + std::string(kThemeNames[i]) + "]" +
                                (view.theme == static_cast<Theme>(i) ? " *" : "");
      draw_text(row, left, cols, label,
                view.theme_selection == i ? selected_style() : body_style());
    }
  }

  void draw_wave(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    const int bottom = top + pane_rows;
    if (top < bottom)
      draw_field(top, left, cols, "Status", view.wave.status, body_style());
    const int list_top = top + 2;
    const int visible = std::max(0, bottom - list_top);
    const size_t start = visible > 0 && view.wave_selection >= static_cast<size_t>(visible) ?
        view.wave_selection - static_cast<size_t>(visible) + 1 : 0;
    for (int i = 0; i < visible && start + static_cast<size_t>(i) < view.wave_stations.size(); ++i) {
      const size_t index = start + static_cast<size_t>(i);
      const std::string label = std::string(view.wave.playing == index ? "* " : "  ") +
                                view.wave_stations[index].name;
      const attr_t style = view.wave_selection == index ?
                           selected_style() : body_style();
      draw_text(list_top + i, left, cols, label, style);
    }
  }

  void draw_audio(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    const int bottom = top + pane_rows;
    if (top < bottom) draw_field(top, left, cols, "Status", view.audio.status,
                                  view.audio.available ? body_style() : muted_style());
    if (!view.audio.available) return;

    for (size_t index = 0; index < 2; ++index) {
      const int row = top + 2 + static_cast<int>(index);
      if (row >= bottom) break;
      const auto &devices = index == 0 ? view.audio.outputs : view.audio.inputs;
      const auto selected = std::find_if(devices.begin(), devices.end(),
          [](const AudioDevice &device) { return device.is_default; });
      const bool focused = !view.menu_focused && view.audio_page &&
                           index == view.audio_selection;
      const attr_t style = focused ? selected_style() : body_style();
      if (focused) draw_text(row, left, cols, "", style, true);
      const bool audio_icon = ghostty_ && cols >= 3;
      const int icon_width = audio_icon ? 3 : 0;
      const std::string label = index == 0 ? "[Output] " : "[Input] ";
      std::string suffix;
      if (selected != devices.end()) {
        const unsigned volume = std::min(selected->volume, 100u);
        const int bar_width = std::clamp(cols - 36, 0, 10);
        if (bar_width > 0) {
          const int filled = (static_cast<int>(volume) * bar_width + 50) / 100;
          suffix = std::string(filled, '|') +
                   std::string(bar_width - filled, ' ') + "  ";
        }
        const std::string percentage = std::to_string(selected->volume) + "%";
        suffix += std::string(std::max(0, 4 - static_cast<int>(percentage.size())), ' ') +
                  percentage + (selected->muted ? " muted" : "      ");
      }
      const int name_width = std::max(0, cols - icon_width - static_cast<int>(suffix.size()) - 1);
      draw_text(row, left + icon_width, name_width, label +
          (selected == devices.end() ? (devices.empty() ? "No device" : "Not selected") :
                                      selected->description), style);
      if (audio_icon) {
        // Nerd Fonts' Font Awesome speaker/microphone glyphs occupy one cell.
        const unsigned volume = selected == devices.end() ? 0u : selected->volume;
        const bool muted = selected != devices.end() && selected->muted;
        const wchar_t *icon = index == 0 ?
            (muted || volume == 0 ? L"\uF026" : volume <= 50 ? L"\uF027" : L"\uF028") :
            (muted ? L"\uF131" : L"\uF130");
        attron(style);
        (void)mvaddnwstr(row, left, icon, 1);
        attroff(style);
      }
      if (!suffix.empty() && static_cast<int>(suffix.size()) < cols)
        draw_text(row, left + cols - static_cast<int>(suffix.size()),
                  static_cast<int>(suffix.size()), suffix, style);
    }
  }

  void draw_bluetooth(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    const int bottom = top + pane_rows;
    if (top < bottom) draw_field(top, left, cols, "Power",
        view.bluetooth.available ? (view.bluetooth.powered ? "On" : "Off") : "Unavailable",
        view.bluetooth.available ? body_style() : muted_style());
    if (top + 1 < bottom) draw_field(top + 1, left, cols, "Status",
        view.bluetooth.status + (view.bluetooth.discovering ? "  scanning" : ""),
        view.bluetooth.available ? body_style() : muted_style());
    const int list_top = top + 3;
    const int list_bottom = bottom;
    if (!view.bluetooth.available || list_top >= list_bottom) return;
    if (view.bluetooth.devices.empty()) {
      draw_text(list_top, left, cols,
          view.bluetooth.powered ? "No devices; press r to scan" : "Bluetooth is off; press w",
          muted_style());
      return;
    }
    const int visible = list_bottom - list_top;
    const size_t selected = std::min(view.bluetooth_selection, view.bluetooth.devices.size() - 1);
    const size_t start = visible > 0 && selected >= static_cast<size_t>(visible) ?
        selected - static_cast<size_t>(visible) + 1 : 0;
    for (int i = 0; i < visible && start + static_cast<size_t>(i) < view.bluetooth.devices.size(); ++i) {
      const auto &device = view.bluetooth.devices[start + static_cast<size_t>(i)];
      const int row = list_top + i;
      const bool icon = ghostty_ && cols >= 3;
      const attr_t style = !view.menu_focused && view.bluetooth_page &&
          start + static_cast<size_t>(i) == selected ? selected_style() : body_style();
      const char *state = device.connected ? "connected" : device.paired ? "paired" : "found";
      const std::string label = std::string(device.connected ? "* " : device.paired ? "+ " : "  ") +
                                device.name + "  " + state + (device.trusted ? "  trusted" : "");
      draw_text(row, left, icon ? cols - 3 : cols, label, style);
      if (icon) {
        // Nerd Fonts' nf-fa-bluetooth is a monochrome, single-cell glyph.
        attron(body_style());
        (void)mvaddnwstr(row, left + cols - 2, L"\uF293", 1);
        attroff(body_style());
      }
    }
  }

  void draw_network(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    const int bottom = top + pane_rows;
    std::string wifi_summary = view.wifi.device.empty() ? "No adapter" :
        view.wifi.device + (view.wifi.enabled ?
            (view.wifi.connected_ssid.empty() ? "  disconnected" :
             "  " + view.wifi.connected_ssid) : "  off");
    if (top < bottom) draw_field(top, left, cols, "WiFi", wifi_summary,
                                 view.wifi.device.empty() ? muted_style() : body_style());
    const auto active_lan = std::find_if(view.wifi.wired.begin(), view.wifi.wired.end(),
        [](const LanConnection &lan) { return lan.connected; });
    const LanConnection *lan = active_lan != view.wifi.wired.end() ? &*active_lan :
        view.wifi.wired.empty() ? nullptr : &view.wifi.wired.front();
    std::string lan_summary = "No Ethernet interface";
    if (lan) {
      lan_summary = lan->interface_name + "  " +
          (lan->connected ? "connected" : !lan->managed ? "unmanaged" :
           !lan->carrier ? "cable unplugged" : lan->connecting ? "connecting" : "disconnected");
      if (lan->connected && !lan->ip_address.empty()) lan_summary += "  " + lan->ip_address;
      if (view.wifi.wired.size() > 1)
        lan_summary += "  (" + std::to_string(view.wifi.wired.size()) + " interfaces)";
    }
    if (top + 1 < bottom) draw_field(top + 1, left, cols, "LAN", lan_summary,
                                     lan ? body_style() : muted_style());
    if (top + 2 < bottom) draw_field(top + 2, left, cols, "Status",
        view.wifi.status + (view.wifi.scanning ? "  scanning" : ""),
        view.wifi.available ? body_style() : muted_style());
    const int list_top = top + 4;
    const int list_bottom = bottom;
    const size_t wired_count = view.wifi.wired.size();
    const size_t total = wired_count + view.wifi.networks.size();
    if (total == 0) {
      if (list_top < list_bottom) draw_text(list_top, left, cols,
          !view.wifi.enabled && !view.wifi.device.empty() ? "WiFi is off; press w to enable" :
          "No wired devices or WiFi networks; press r to refresh",
          muted_style());
      return;
    }
    const int available_rows = std::max(0, list_bottom - list_top);
    const size_t start = view.wifi_selection >= static_cast<size_t>(available_rows) && available_rows > 0 ?
        view.wifi_selection - static_cast<size_t>(available_rows) + 1 : 0;
    for (int i = 0; i < available_rows && start + static_cast<size_t>(i) < total; ++i) {
      const size_t index = start + static_cast<size_t>(i);
      const attr_t style = !view.menu_focused && view.wifi_page && index == view.wifi_selection ?
                           selected_style() : body_style();
      if (index < wired_count) {
        const auto &wired = view.wifi.wired[index];
        std::string label = std::string(wired.connected ? "* LAN  " : "  LAN  ") +
                            wired.interface_name + "  " +
            (wired.connected ? "connected" : !wired.managed ? "unmanaged" :
             !wired.carrier ? "no cable" : wired.connecting ? "connecting" : "disconnected");
        if (wired.connected && !wired.ip_address.empty()) label += "  " + wired.ip_address;
        if (wired.connected && wired.speed_mbps) label += "  " +
            std::to_string(wired.speed_mbps) + " Mb/s";
        draw_text(list_top + i, left, cols, label, style);
        continue;
      }
      const auto &network = view.wifi.networks[index - wired_count];
      const bool lock_icon = ghostty_ && network.visible &&
                             network.security == WifiSecurity::Personal && cols >= 3;
      const char *security = network.security == WifiSecurity::Open ? "open" :
                             network.security == WifiSecurity::Personal ? "lock" :
                             network.security == WifiSecurity::Enterprise ? "802.1X" : "other";
      std::string label = network.connected ? "* WiFi " : "  WiFi ";
      label += network.ssid;
      if (network.visible) {
        label += "  " + std::to_string(network.strength) + "%";
        if (!lock_icon) label += "  " + std::string(security);
      }
      else label += "  out of range";
      if (!network.saved_uuid.empty()) label += " saved";
      draw_text(list_top + i, left, lock_icon ? cols - 3 : cols, label, style);
      if (lock_icon) {
        // Keep the right-aligned icon on the page background. The row highlight
        // ends with its text, so highlighting only the icon makes a stray block.
        attron(body_style());
        // Nerd Fonts' nf-fa-lock stays monochrome and occupies one terminal cell.
        (void)mvaddnwstr(list_top + i, left + cols - 2, L"\uF023", 1);
        attroff(body_style());
      }
    }
  }

  void draw_footer(int row, int left, int cols, const std::string &status) {
    constexpr const char *brand = "CYBER DECK";
    const int brand_col = std::max(0, cols - static_cast<int>(std::strlen(brand)));
    if (brand_col > 7) draw_text(row, left + brand_col, cols - brand_col, brand, section_style());
    const int middle_width = brand_col - 1;
    if (middle_width <= 0) return;
    if (!status.empty()) {
      draw_text(row, left, middle_width, status, muted_style());
    }
  }

  short color_index(VTermColor color, bool foreground) const {
    if (foreground ? VTERM_COLOR_IS_DEFAULT_FG(&color) : VTERM_COLOR_IS_DEFAULT_BG(&color)) {
      if (theme_ == Theme::Greenline)
        return COLORS >= 256 ? (foreground ? 194 : 232) :
               (foreground ? (COLORS >= 16 ? 10 : COLOR_GREEN) : COLOR_BLACK);
      if (theme_ == Theme::Synthwave)
        return COLORS >= 256 ? (foreground ? 225 : 17) :
               (foreground ? COLOR_MAGENTA : COLOR_BLACK);
      return default_colors_ ? -1 : (foreground ? COLOR_WHITE : COLOR_BLACK);
    }
    if (VTERM_COLOR_IS_INDEXED(&color)) {
      int index = color.indexed.idx;
      if (index < 16) return static_cast<short>(index % 8);
      if (index >= 232) return index >= 244 ? COLOR_WHITE : COLOR_BLACK;
      int cube = index - 16;
      return static_cast<short>(((cube / 36) >= 3 ? 1 : 0) * COLOR_RED +
                                (((cube / 6) % 6) >= 3 ? 1 : 0) * COLOR_GREEN +
                                ((cube % 6) >= 3 ? 1 : 0) * COLOR_BLUE);
    }
    return static_cast<short>((color.rgb.red >= 128 ? COLOR_RED : 0) +
                              (color.rgb.green >= 128 ? COLOR_GREEN : 0) +
                              (color.rgb.blue >= 128 ? COLOR_BLUE : 0));
  }

  short color_pair(VTermColor fg, VTermColor bg) {
    if (!has_colors() || COLORS < 8) return 0;
    short foreground = color_index(fg, true);
    short background = color_index(bg, false);
    auto key = std::make_pair(foreground, background);
    auto found = pairs_.find(key);
    if (found != pairs_.end()) return found->second;
    if (next_pair_ >= COLOR_PAIRS || next_pair_ >= 81) return 0;
    short pair = next_pair_++;
    if (init_pair(pair, foreground, background) != OK) return 0;
    pairs_[key] = pair;
    return pair;
  }

  bool default_colors_ = false;
  bool chrome_colors_ = false;
  UiLayout layout_ = ui_layout();
  bool ghostty_ = false;
  bool host_background_changed_ = false;
  Theme theme_ = Theme::Husky;
  std::map<std::pair<short, short>, short> pairs_;
  short next_pair_ = 5;
};

}  // namespace cyberdeck
