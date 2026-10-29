#pragma once

#include "audio.hpp"
#include "bluetooth.hpp"
#include "cookies.hpp"
#include "hardware.hpp"
#include "preferences.hpp"
#include "system.hpp"
#include "terminal.hpp"
#include "theme.hpp"
#include "weather.hpp"
#include "wifi.hpp"
#include "wave.hpp"
#include "web.hpp"

#include <ctime>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cyberdeck {

constexpr std::array<const char *, 6> kTabs = {
    "Dashboard", "Terminal", "Media", "Notes", "Web", "Settings"};
constexpr std::array<const char *, 4> kSettingsSections = {
    "General", "Interface", "Devices", "Apps"};
enum class HelpDialog { None, Confirm, Password, TextEntry };
enum class TextPrompt { None, StationName, StationUrl, NoteName, WebQuery, WeatherCity };

struct ChromeView {
  size_t tab = 0;
  size_t menu_selection = 0;
  bool menu_focused = true;
  HelpDialog help_dialog = HelpDialog::None;
  TextPrompt text_prompt = TextPrompt::None;
  Theme theme = Theme::Husky;
  size_t settings_selection = 0;
  size_t settings_item = 0;
  std::array<bool, 2> general_expanded{};
  std::array<bool, 3> interface_expanded{};
  std::array<bool, 5> device_expanded{};
  bool device_content_focused = false;
  bool vi_keys = false;
  bool unicode_glyphs = true;
  std::array<bool, 4> dashboard_widgets{{true, true, true, true}};
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
  bool wifi_picker = false;
  bool bluetooth_picker = false;
  bool hardware_details_open = false;
  size_t storage_selection = 0;
  size_t peripheral_selection = 0;
  HardwareSnapshot hardware;
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
  WeatherSnapshot weather;
  std::string notice;
  std::array<bool, kMaxSessions> terminals{};
  size_t active_terminal = 0;
};

struct ModalView {
  enum class Kind { None, Confirm, Password, Info, AudioPicker, NetworkPicker,
                    BluetoothPicker, HardwareDetails, TextEntry } kind = Kind::None;
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
    menu_color_ = false;
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
      const short selection_background = extended ? 22 : COLOR_GREEN;
      if (chrome_colors_ && COLOR_PAIRS > 5 &&
          init_pair(5, selection_background, background) == OK) {
        menu_color_ = true;
        next_pair_ = 6;
      }
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
    if (chrome_colors_ && COLOR_PAIRS > 5 && init_pair(5, selection, background) == OK) {
      menu_color_ = true;
      next_pair_ = 6;
    }
  }

  void draw(TerminalModel *model, const ChromeView &view,
            const std::string &status, const ModalView &modal) {
    unicode_glyphs_ = view.unicode_glyphs;
    int host_rows = 0, host_cols = 0;
    getmaxyx(stdscr, host_rows, host_cols);
    if (modal.kind != ModalView::Kind::None) {
      (void)wbkgdset(stdscr, static_cast<chtype>(' ') | body_style());
      erase();
      if (modal.kind == ModalView::Kind::AudioPicker)
        draw_audio_picker(view, host_rows, host_cols);
      else if (modal.kind == ModalView::Kind::NetworkPicker)
        draw_network_picker(view, host_rows, host_cols);
      else if (modal.kind == ModalView::Kind::BluetoothPicker)
        draw_bluetooth_picker(view, host_rows, host_cols);
      else if (modal.kind == ModalView::Kind::HardwareDetails)
        draw_hardware_details(view, host_rows, host_cols);
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
  attr_t menu_selected_style() const {
    return menu_color_ ? A_BOLD | COLOR_PAIR(5) : section_style();
  }
  attr_t menu_selected_text_style() const {
    return section_style();
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
    std::vector<Shortcut> global = {
        {"Alt+d", "Dashboard"}, {"Alt+t", "Terminal"},
        {"Alt+m", "Media"},
        {"Alt+w", "Web"}, {"Alt+s", "Settings"},
        {"Alt+q", "Quit"}, {"Alt+Esc", "Back / cancel"},
        {"Shift+Alt+?", "Toggle Help"}};
    if (view.help_dialog == HelpDialog::None || view.help_dialog == HelpDialog::TextEntry)
      global.insert(global.begin() + 3, {"Alt+n", "Notes"});
    std::string section;
    std::vector<Shortcut> local;
    std::string note;
    if (view.help_dialog == HelpDialog::Confirm) {
      section = "Confirmation";
      local = {{"Left", "Select Yes"}, {"Right", "Select No"},
               {"Tab", "Change choice"}, {"Enter", "Use choice"},
               {"Alt+y", "Yes"}, {"Alt+n", "No"}};
      if (view.vi_keys) {
        local.emplace_back("h", "Select Yes");
        local.emplace_back("l", "Select No");
      }
    } else if (view.help_dialog == HelpDialog::Password) {
      section = "Password prompt";
      local = {{"Enter", "Submit"}, {"Backspace", "Delete"},
               {"Left", "Choose action"}, {"Right", "Choose action"}};
    } else if (view.help_dialog == HelpDialog::TextEntry) {
      section = view.text_prompt == TextPrompt::StationName ? "Add station / Name" :
          view.text_prompt == TextPrompt::StationUrl ? "Add station / Stream URL" :
          view.text_prompt == TextPrompt::NoteName ? "New note / Name" :
          view.text_prompt == TextPrompt::WebQuery ? "Web / Search or URL" :
          view.text_prompt == TextPrompt::WeatherCity ? "General / Weather / City" : "Text prompt";
      const char *submit = view.text_prompt == TextPrompt::StationName ? "Next: stream URL" :
          view.text_prompt == TextPrompt::NoteName ? "Create note" :
          view.text_prompt == TextPrompt::WebQuery ? "Open page" : "Save";
      local = {{"Enter", submit}, {"Backspace", "Delete"},
               {"Ctrl+u", "Clear entry"}};
    } else if (view.menu_focused) {
      section = std::string(kTabs[view.tab]) + " preview";
      if (view.tab > 0) local.emplace_back("Alt+h/Left", "Previous");
      if (view.tab + 1 < kTabs.size()) local.emplace_back("Alt+l/Right", "Next");
      if (view.tab != 0) local.emplace_back("Enter", "Open module");
    } else if (view.tab == 0) {
      section = "Dashboard";
    } else if (view.tab == 1) {
      section = "Terminal";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next / new tab"},
               {"Alt+Tab", "New terminal"},
               {"Alt+1", "Terminal 1"}, {"Alt+2", "Terminal 2"},
               {"Alt+3", "Terminal 3"}, {"Alt+4", "Terminal 4"},
               {"Alt+x", "Close tab if 2+"},
               {"Tab/Shift+Tab", "Send to shell"},
               {"Ctrl+c", "Interrupt"}, {"Ctrl+z", "Suspend"},
               {"Ctrl+d", "Exit shell"}};
    } else if (view.tab == 2) {
      section = "Media / Wave";
      local = {{view.vi_keys ? "j/Down" : "Down", "Next station"},
               {view.vi_keys ? "k/Up" : "Up", "Previous station"},
               {"Enter", "Play / stop"}, {"a", "Add station"},
               {"s", "Stop playback"}};
    } else if (view.tab == 3 && view.notes_section == 0) {
      section = "Notes / Text";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next tab"}};
      if (view.note_editing)
        local.insert(local.end(), {{"Ctrl+o", "Save in nano"}, {"Ctrl+x", "Exit nano"}});
      else
        local.insert(local.end(), {{"n", "New note"},
                 {view.vi_keys ? "j/Down" : "Down", "Next note"},
                 {view.vi_keys ? "k/Up" : "Up", "Previous note"},
                 {"Enter", "Edit note"}});
    } else if (view.tab == 3) {
      section = "Notes / Voice";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next tab"}};
      note = "Recording is not available yet";
    } else if (view.tab == 4) {
      section = "Web";
      local = {{"Alt+h", "Previous tab"}, {"Alt+l", "Next / new tab"},
               {"Alt+Tab", "New Web tab"}, {"Alt+x", "Close tab if 2+"}};
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
    } else if (view.tab == 5 && view.hardware_details_open) {
      section = "Settings / Device details";
      local = {{"Alt+Esc", "Close details"}};
    } else if (view.tab == 5 && view.settings_selection == 0 && view.settings_item <= 1) {
      section = "Settings / General / Navigation";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Move through settings"},
               {"Enter", view.settings_item == 0 ? "Expand / collapse" : "Toggle vi motion"}};
    } else if (view.tab == 5 && view.settings_selection == 0) {
      section = "Settings / General / Weather";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Move through settings"},
               {"Enter", view.settings_item == 2 ? "Expand / collapse" : "Set city"},
               {"r", "Refresh weather"}};
    } else if (view.tab == 5 && view.settings_selection == 1) {
      section = "Settings / Interface";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Move through options"},
               {"Enter", view.settings_item == 0 || view.settings_item == 4 ||
                   view.settings_item == 9 ? "Expand / collapse" :
                   view.settings_item < 4 ? "Apply theme" :
                   view.settings_item < 9 ? "Toggle widget" : "Choose glyph mode"}};
    } else if (view.tab == 5 && view.wifi_picker) {
      section = "Settings / Network connections";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Select connection"},
               {"Enter", "Connect selected"}, {"r", "Scan WiFi"},
               {"d", "Disconnect"}, {"w", "Toggle WiFi radio"},
               {"p", "Replace password"}, {"f", "Forget WiFi"}};
    } else if (view.tab == 5 && view.bluetooth_picker) {
      section = "Settings / Bluetooth devices";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Select device"},
               {"Enter", "Connect / pair"}, {"r", "Scan"},
               {"w", "Power on/off"}, {"p", "Pair"}, {"d", "Disconnect"},
               {"t", "Trust / untrust"}, {"f", "Forget"}};
    } else if (view.tab == 5 && view.audio_picker) {
      section = "Settings / Audio picker";
      local = {{view.vi_keys ? "j/Down" : "Down", "Next device"},
               {view.vi_keys ? "k/Up" : "Up", "Previous device"},
               {"Enter", "Use device"}};
    } else if (view.tab == 5 && view.settings_selection == 2 && !view.device_content_focused) {
      section = "Settings / Devices";
      local = {{"1-5", "Select section"},
               {view.vi_keys ? "j/k + arrows" : "Up/Down", "Select section"},
               {"Enter", "Expand / collapse"}};
      if (view.device_expanded[view.settings_item])
        local.emplace_back("Down", "Focus expanded content");
      if (view.settings_item > 0 && view.device_expanded[view.settings_item - 1])
        local.emplace_back("Up", "Last item above");
      if (view.settings_item >= 3) local.emplace_back("r", "Refresh hardware");
    } else if (view.tab == 5 && view.settings_selection == 2 && view.settings_item == 0) {
      section = "Settings / Devices / Network";
      local = {{"Enter", "Browse WiFi and LAN"}, {"r", "Scan WiFi"},
               {"w", "Toggle WiFi radio"}, {"Down", "Next section"},
               {"Alt+Esc", "Back to section"}};
    } else if (view.tab == 5 && view.settings_selection == 2 && view.settings_item == 1) {
      section = "Settings / Devices / Audio";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Select audio control"},
               {"Enter", "Choose device"}, {view.vi_keys ? "h/Left" : "Left", "Volume down"},
               {view.vi_keys ? "l/Right" : "Right", "Volume up"}, {"r", "Refresh"},
               {"Down on Input", "Next section"}};
    } else if (view.tab == 5 && view.settings_selection == 2 && view.settings_item == 2) {
      section = "Settings / Devices / Bluetooth";
      local = {{"Enter", "Add / pair device"}, {"r", "Scan"},
               {"w", "Power on/off"}, {"Down", "Next section"},
               {"Alt+Esc", "Back to section"}};
    } else if (view.tab == 5 && view.settings_selection == 2 && view.settings_item == 3) {
      section = "Settings / Devices / Storage";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Browse drives"},
               {"Enter", "Device details"}, {"r", "Refresh"},
               {"Down at end", "Next section"},
               {"Alt+Esc", "Back to section"}};
    } else if (view.tab == 5 && view.settings_selection == 2 && view.settings_item == 4) {
      section = "Settings / Devices / Peripherals";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Browse USB devices"},
               {"Enter", "Device details"}, {"r", "Refresh"},
               {"Alt+Esc", "Back to section"}};
    } else if (view.tab == 5 && view.settings_selection == 3) {
      section = "Settings / Apps / Web";
      local = {{view.vi_keys ? "j/k + arrows" : "Up/Down", "Move through options"},
               {"Enter", "Apply / toggle option"}};
    }
    if (view.tab == 5 && !view.menu_focused && !view.hardware_details_open &&
        !view.audio_picker && !view.wifi_picker && !view.bluetooth_picker) {
      bool collapsible = false;
      if (view.settings_selection == 0)
        collapsible = view.general_expanded[view.settings_item < 2 ? 0 : 1];
      else if (view.settings_selection == 1)
        collapsible = view.interface_expanded[
            view.settings_item < 4 ? 0 : view.settings_item < 9 ? 1 : 2];
      else if (view.settings_selection == 2)
        collapsible = view.device_expanded[view.settings_item] &&
                      !(view.settings_item == 1 && view.device_content_focused);
      if (collapsible) local.emplace_back("h", "Collapse section");
    }
    if (view.help_dialog == HelpDialog::None && !view.menu_focused &&
        !view.hardware_details_open && !view.audio_picker &&
        !view.wifi_picker && !view.bluetooth_picker) {
      size_t position = 0, count = 0;
      if (view.tab == 3) { position = view.notes_section; count = 2; }
      else if (view.tab == 4) {
        position = view.web_tab;
        count = static_cast<size_t>(std::count(view.web_tabs.begin(), view.web_tabs.end(), true));
      } else if (view.tab == 5) {
        position = view.settings_selection;
        count = kSettingsSections.size();
        local.emplace_back("Alt+h/Alt+l", "Previous/next section");
      }
      const char *previous = view.tab == 5 ? "Previous section" : "Previous top tab";
      const char *next = view.tab == 5 ? "Next section" : "Next top tab";
      if (count && position > 0) local.emplace_back("Shift+Tab", previous);
      if (count && position + 1 < count) local.emplace_back("Tab", next);
    }
    if (view.help_dialog == HelpDialog::None) {
      if (!view.hardware_details_open && !view.audio_picker && !view.wifi_picker && !view.bluetooth_picker &&
          !view.menu_focused) {
        if (view.tab == 1) local.emplace_back("Esc", "Send to shell");
        else if (view.tab == 3 && view.note_editing)
          local.emplace_back("Esc", "Send to nano");
        else if (view.tab == 4 && view.web_browsing)
          local.emplace_back("Esc", "ELinks menu");
      }
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
  }

  void draw_modal(const ModalView &modal, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    const int center = rows / 2;
    if (modal.kind == ModalView::Kind::TextEntry) {
      draw_centered(center - 2, cols, modal.message, section_style());
      draw_centered(center, cols, modal.detail, modal_style(), std::max(1, cols - 4));
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

  void draw_hardware_details(const ChromeView &view, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    std::vector<std::string> lines;
    std::string title;
    if (view.settings_item == 3 && view.storage_selection < view.hardware.drives.size()) {
      const auto &drive = view.hardware.drives[view.storage_selection];
      title = "Storage details";
      lines.push_back("Device: /dev/" + drive.name);
      if (!drive.model.empty()) lines.push_back("Model: " + drive.model);
      lines.push_back("Mount: " + (drive.mountpoint.empty() ? "Not mounted" : drive.mountpoint));
      if (!drive.filesystem.empty()) lines.push_back("Filesystem: " + drive.filesystem);
      if (!drive.mountpoint.empty())
        lines.push_back("Free: " + storage_size(drive.free_bytes));
      lines.push_back("Total: " + storage_size(drive.size_bytes));
      if (drive.usb) lines.push_back("Connection: USB");
    } else if (view.settings_item == 4 &&
               view.peripheral_selection < view.hardware.usb_devices.size()) {
      const auto &device = view.hardware.usb_devices[view.peripheral_selection];
      title = "Peripheral details";
      lines.push_back("Name: " + device.name);
      if (!device.manufacturer.empty() && device.manufacturer != device.name)
        lines.push_back("Maker: " + device.manufacturer);
      lines.push_back("USB ID: " + device.vendor_id + ":" + device.product_id);
      if (!device.serial.empty()) lines.push_back("Serial: " + device.serial);
      if (!device.speed.empty()) lines.push_back("Speed: " + device.speed + " Mb/s");
      if (!device.port.empty()) lines.push_back("Port: " + device.port);
    } else return;
    const int width = std::max(1, cols - 4);
    const int top = std::max(0, (rows - static_cast<int>(lines.size()) - 2) / 2);
    draw_centered(top, cols, title, section_style(), width);
    int line_width = 0;
    for (const auto &line : lines)
      line_width = std::max(line_width, std::min(width, static_cast<int>(line.size())));
    const int left = std::max(0, (cols - line_width) / 2);
    for (size_t i = 0; i < lines.size() && top + 2 + static_cast<int>(i) < rows - 1; ++i)
      draw_text(top + 2 + static_cast<int>(i), left, std::min(width, cols - left),
                lines[i], modal_style());
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

  void draw_network_picker(const ChromeView &view, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    const size_t wired = view.wifi.wired.size();
    const size_t total = wired + view.wifi.networks.size();
    const int visible = std::min<int>(static_cast<int>(total),
                                     std::min(10, std::max(1, rows - 5)));
    const int top = std::max(0, (rows - visible - 3) / 2);
    const int width = std::max(1, cols - 4);
    draw_centered(top, cols, "Select connection", section_style(), width);
    if (total == 0)
      draw_centered(top + 2, cols, "No connections found", muted_style(), width);
    else {
      const size_t selected = std::min(view.wifi_selection, total - 1);
      const size_t start = selected >= static_cast<size_t>(visible) ?
          selected - static_cast<size_t>(visible) + 1 : 0;
      for (int row = 0; row < visible; ++row) {
        const size_t index = start + static_cast<size_t>(row);
        if (index >= total) break;
        std::string label;
        if (index < wired) {
          const auto &lan = view.wifi.wired[index];
          label = std::string(lan.connected ? "* " : "  ") + "LAN " + lan.interface_name +
                  (lan.connected ? "  connected" : lan.carrier ? "  available" : "  no cable");
        } else {
          const auto &network = view.wifi.networks[index - wired];
          label = std::string(network.connected ? "* " : "  ") + network.ssid;
          label += network.visible ? "  " + std::to_string(network.strength) + "%" : "  out of range";
          if (network.security == WifiSecurity::Personal) label += "  lock";
          if (!network.saved_uuid.empty()) label += "  saved";
        }
        draw_centered(top + 2 + row, cols, label,
                      index == selected ? modal_selected_style() : modal_style(), width);
      }
    }
    if (!view.notice.empty())
      draw_centered(top + visible + 3, cols, view.notice, section_style(), width);
  }

  void draw_bluetooth_picker(const ChromeView &view, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return;
    const auto &devices = view.bluetooth.devices;
    const int visible = std::min<int>(static_cast<int>(devices.size()),
                                     std::min(10, std::max(1, rows - 8)));
    const int top = std::max(0, (rows - visible - 3) / 2);
    const int width = std::max(1, cols - 4);
    draw_centered(top, cols, "Select Bluetooth device", section_style(), width);
    if (devices.empty())
      draw_centered(top + 2, cols, "No devices found", muted_style(), width);
    else {
      const size_t selected = std::min(view.bluetooth_selection, devices.size() - 1);
      const size_t start = selected >= static_cast<size_t>(visible) ?
          selected - static_cast<size_t>(visible) + 1 : 0;
      for (int row = 0; row < visible; ++row) {
        const size_t index = start + static_cast<size_t>(row);
        if (index >= devices.size()) break;
        const auto &device = devices[index];
        const char *state = device.connected ? "connected" : device.paired ? "paired" : "found";
        const std::string label = std::string(device.connected ? "* " : "  ") +
                                  device.name + "  " + state;
        draw_centered(top + 2 + row, cols, label,
                      index == selected ? modal_selected_style() : modal_style(), width);
      }
    }
    const std::string status = view.bluetooth.status.rfind("Bluetooth:", 0) == 0 ||
        view.bluetooth.status == "Done" || view.bluetooth.status.rfind("Connected to ", 0) == 0 ||
        view.bluetooth.status.rfind("Paired with ", 0) == 0 ? view.bluetooth.status :
        !view.notice.empty() ? view.notice : view.bluetooth.status;
    const std::string message = "Status: " + status;
    const int status_top = top + visible + 3;
    for (int line = 0; line < 3 && status_top + line < rows - 1; ++line) {
      const size_t offset = static_cast<size_t>(line) * static_cast<size_t>(width);
      if (offset >= message.size()) break;
      draw_centered(status_top + line, cols, message.substr(offset, width),
                    section_style(), width);
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

  static std::string bracketed_value(const std::string &value, int width) {
    if (width < 2) return {};
    size_t end = std::min(value.size(), static_cast<size_t>(width - 2));
    if (end < value.size())
      while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80)
        --end;
    return "[" + value.substr(0, end) + "]";
  }

  void draw_preview(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (pane_rows <= 0 || cols <= 0) return;
    if (view.tab == 0) {
      draw_page(view, pane_rows, left, top, cols);
      return;
    }
    std::string title;
    for (const char character : std::string(kTabs[view.tab])) {
      if (!title.empty()) title += ' ';
      title += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    const int width = std::max(25, static_cast<int>(title.size()) + 8);
    if (pane_rows >= 3 && width <= cols - 2) {
      const int y = top + (pane_rows - 3) / 2;
      const int x = left + (cols - width) / 2;
      draw_widget_glyph(y, x, L'╭', muted_style());
      draw_widget_glyph(y, x + width - 1, L'╮', muted_style());
      draw_widget_glyph(y + 2, x, L'╰', muted_style());
      draw_widget_glyph(y + 2, x + width - 1, L'╯', muted_style());
      for (int col = 1; col < width - 1; ++col) {
        draw_widget_glyph(y, x + col, L'─', muted_style());
        draw_widget_glyph(y + 2, x + col, L'─', muted_style());
      }
      draw_widget_glyph(y + 1, x, L'│', muted_style());
      draw_widget_glyph(y + 1, x + width - 1, L'│', muted_style());
      draw_text(y + 1, x + (width - static_cast<int>(title.size())) / 2,
                static_cast<int>(title.size()), title, title_style());
    } else {
      const std::string label = "[" + std::string(kTabs[view.tab]) + "]";
      const int offset = std::max(0, (cols - static_cast<int>(label.size())) / 2);
      draw_text(top + pane_rows / 2, left + offset, cols - offset, label, title_style());
    }
  }

  void draw_content_menu(const ChromeView &view, int row, int left, int cols) {
    std::vector<std::string> labels;
    size_t selected = 0;
    if (view.tab == 1) {
      for (size_t i = 0; i < kMaxSessions; ++i) {
        if (!view.terminals[i]) continue;
        if (i == view.active_terminal) selected = labels.size();
        labels.push_back(std::to_string(i + 1));
      }
    } else if (view.tab == 2) {
      labels.emplace_back("Wave");
    } else if (view.tab == 3) {
      labels = {"Text", "Voice"};
      selected = view.notes_section;
    } else if (view.tab == 4) {
      for (size_t i = 0; i < view.web_tabs.size(); ++i) {
        if (!view.web_tabs[i]) continue;
        labels.push_back(std::to_string(i + 1));
      }
      selected = view.web_tab;
    } else if (view.tab == 5) {
      labels.assign(kSettingsSections.begin(), kSettingsSections.end());
      selected = view.settings_selection;
    } else {
      labels.emplace_back(kTabs[view.tab]);
    }
    if (labels.empty()) labels.emplace_back(kTabs[view.tab]);
    draw_box_menu(labels, selected, row, left, cols);
    draw_help_hint(row + 3, left, cols, 0);
  }

  void draw_box_menu(const std::vector<std::string> &labels, size_t selected,
                     int row, int left, int cols) {
    if (cols <= 0 || labels.empty()) return;
    auto box_width = [&](size_t index) {
      return static_cast<int>(labels[index].size()) + 4;
    };
    const int count = static_cast<int>(labels.size());
    int total = count - 1;
    for (int i = 0; i < count; ++i) total += box_width(static_cast<size_t>(i));
    int first = 0;
    while (first < static_cast<int>(selected)) {
      int through_selected = static_cast<int>(selected) - first;
      for (int i = first; i <= static_cast<int>(selected); ++i)
        through_selected += box_width(static_cast<size_t>(i));
      if (through_selected <= cols) break;
      ++first;
    }
    const int start = total <= cols ? left + (cols - total) / 2 : left;
    int col = start;
    for (int i = first; i < count; ++i) {
      const int width = box_width(static_cast<size_t>(i));
      if (col + width > left + cols) break;
      const bool active = static_cast<size_t>(i) == selected;
      const attr_t border = muted_style();
      if (active) {
        draw_widget_glyph(row, col, L'╭', border);
        draw_widget_glyph(row, col + width - 1, L'╮', border);
        draw_widget_glyph(row + 1, col, L'│', border);
        draw_widget_glyph(row + 1, col + width - 1, L'│', border);
        draw_widget_glyph(row + 2, col, L'╰', border);
        draw_widget_glyph(row + 2, col + width - 1, L'╯', border);
        for (int x = 1; x < width - 1; ++x)
          draw_widget_glyph(row, col + x, L'─', border);
        for (int x = 1; x < width - 1; ++x)
          draw_widget_glyph(row + 2, col + x, L'─', border);
      } else {
        for (int x = 1; x < width - 1; ++x)
          draw_widget_glyph(row + 2, col + x, L'─', border);
      }
      draw_text(row + 1, col + 1, width - 2,
                " " + labels[static_cast<size_t>(i)] + " ",
                active ? menu_selected_text_style() : body_style());
      col += width + 1;
    }
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
    if (tab == 0) {
      draw_dashboard(view, pane_rows, left, top, cols);
      return;
    }
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
    const std::string heading = tab == 1 ? "TERMINAL" :
                                std::string(kTabs[tab]);
    draw_text(top, left, cols, heading, title_style());
    if (pane_rows > 1) {
      attron(muted_style());
      (void)mvhline(top + 1, left, '-', cols);
      attroff(muted_style());
    }
    if (tab == 1) {
      if (pane_rows > 3) draw_text(top + 3, left, cols,
          "No terminal is running.", muted_style());
    } else if (pane_rows > 3) {
      draw_text(top + 3, left, cols, "PLANNED", section_style());
    }
  }

  void draw_widget_glyph(int row, int col, wchar_t glyph, attr_t style) {
    cchar_t cell{};
    const wchar_t text[] = {glyph, L'\0'};
    if (unicode_glyphs_ && MB_CUR_MAX > 1 &&
        setcchar(&cell, text, style & ~A_COLOR, PAIR_NUMBER(style), nullptr) == OK &&
        mvadd_wch(row, col, &cell) == OK) return;
    const char fallback = glyph == L'▌' || glyph == L'▸' ? '>' : glyph == L'▾' ? 'v' :
                          glyph == L'─' ? '-' :
                          glyph == L'│' ? '|' :
                          glyph == L'█' ? '#' : glyph == L'░' ? '.' : '+';
    attron(style);
    (void)mvaddch(row, col, fallback);
    attroff(style);
  }

  void draw_widget(int row, int col, int width, const char *title,
                   const std::string &value, const std::string &detail,
                   const std::string &indicator = {}) {
    if (width < 12) return;
    const attr_t border = muted_style();
    draw_widget_glyph(row, col, L'╭', border);
    draw_widget_glyph(row, col + width - 1, L'╮', border);
    draw_widget_glyph(row + 4, col, L'╰', border);
    draw_widget_glyph(row + 4, col + width - 1, L'╯', border);
    for (int x = 1; x < width - 1; ++x) {
      draw_widget_glyph(row, col + x, L'─', border);
      draw_widget_glyph(row + 4, col + x, L'─', border);
    }
    for (int offset = 1; offset < 4; ++offset) {
      draw_widget_glyph(row + offset, col, L'│', border);
      draw_widget_glyph(row + offset, col + width - 1, L'│', border);
    }
    draw_text(row, col + 2, width - 4, " " + std::string(title) + " ", section_style());
    if (!indicator.empty() && width >= static_cast<int>(indicator.size()) + 21)
      draw_text(row + 1, col + width - 3 - static_cast<int>(indicator.size()),
                static_cast<int>(indicator.size()), indicator, section_style());
    draw_text(row + 2, col + 3, width - 6, value, title_style());
    draw_text(row + 3, col + 3, width - 6, detail, muted_style());
  }

  void draw_battery_meter(int row, int col, int width, const std::string &capacity) {
    if (capacity.empty() || capacity.back() != '%') return;
    char *end = nullptr;
    const long percent = std::strtol(capacity.c_str(), &end, 10);
    if (end != capacity.c_str() + capacity.size() - 1 || percent < 0 || percent > 100)
      return;
    const int cells = std::min(12, width - 6);
    if (cells <= 0) return;
    const int filled = static_cast<int>((percent * cells + 50) / 100);
    for (int i = 0; i < cells; ++i)
      draw_widget_glyph(row + 1, col + 3 + i, i < filled ? L'█' : L'░',
                        i < filled ? section_style() : muted_style());
  }

  void draw_dashboard(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    std::string network_value = "Offline";
    std::string network_detail = "No active connection";
    const auto connected_lan = std::find_if(view.wifi.wired.begin(), view.wifi.wired.end(),
        [](const LanConnection &lan) { return lan.connected; });
    const auto default_lan = std::find_if(view.wifi.wired.begin(), view.wifi.wired.end(),
        [&](const LanConnection &lan) {
          return lan.connected && lan.interface_name == view.overview.network_interface;
        });
    const bool wifi_connected = !view.wifi.connected_ssid.empty();
    if (default_lan != view.wifi.wired.end()) {
      const auto &lan = *default_lan;
      network_value = "LAN / " + lan.interface_name;
      network_detail = lan.ip_address.empty() ? "Connected" : lan.ip_address;
    } else if (wifi_connected) {
      network_value = view.wifi.connected_ssid;
      network_detail = "WiFi / " + view.wifi.device;
    } else if (connected_lan != view.wifi.wired.end()) {
      network_value = "LAN / " + connected_lan->interface_name;
      network_detail = connected_lan->ip_address.empty() ? "Connected" : connected_lan->ip_address;
    } else if (!view.overview.network_interface.empty()) {
      network_value = view.overview.network_interface;
      network_detail = "Active network link";
    }

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    char clock[32] = "--:--";
    char date[24] = "Local time";
    if (localtime_r(&now, &local)) {
      (void)std::strftime(clock, sizeof(clock), "%H:%M %p", &local);
      (void)std::strftime(date, sizeof(date), "%a %d %b", &local);
    }
    const size_t battery_separator = view.overview.battery.find("  ");
    const std::string battery_value = view.overview.battery.substr(0, battery_separator);
    const std::string battery_detail = battery_separator == std::string::npos ?
        (battery_value == "Not detected" ? "No system battery" : "Power source") :
        view.overview.battery.substr(battery_separator + 2);
    const std::string weather_value = view.weather.temperature.empty() ?
        view.weather.status : view.weather.temperature;
    const std::string weather_detail = !view.weather.condition.empty() ? view.weather.condition :
        view.weather.location.empty() ? "Settings / General / Weather" :
        view.weather.loading ? "Fetching current weather" :
        view.weather.status == "City not found" ? "Check city name" : "Check network";

    if (cols >= 44 && pane_rows >= 11) {
      const int width = std::min(cols - 4, 72);
      const int col = left + (cols - width) / 2;
      const int enabled = static_cast<int>(std::count(view.dashboard_widgets.begin(),
                                                       view.dashboard_widgets.end(), true));
      const int content_height = enabled > 2 ? 11 : 5;
      const int row = top + (pane_rows - content_height) / 2;
      const int left_width = (width - 2) / 2;
      const int right_col = col + left_width + 2;
      const int right_width = width - left_width - 2;
      const std::array<std::string, 4> values = {clock, weather_value, network_value, battery_value};
      const std::array<std::string, 4> details = {date, weather_detail, network_detail, battery_detail};
      const std::array<const char *, 4> names = {"CLOCK", "WEATHER", "NETWORK", "BATTERY"};
      int shown = 0;
      for (size_t i = 0; i < view.dashboard_widgets.size(); ++i) {
        if (!view.dashboard_widgets[i]) continue;
        const bool wide_single = enabled == 1;
        const int item_width = wide_single ? width : ((shown % 2 == 0) ? left_width : right_width);
        const int item_col = wide_single ? col : ((shown % 2 == 0) ? col : right_col);
        const int item_row = row + (enabled > 2 ? (shown / 2) * 6 : 0);
        draw_widget(item_row, item_col, item_width, names[i], values[i], details[i]);
        if (i == 1 && !view.weather.location.empty())
          draw_text(item_row + 1, item_col + 3, item_width - 6, view.weather.location, section_style());
        if (i == 2)
          draw_text(item_row + 1, item_col + 3, item_width - 6,
                    network_value == "Offline" ? "Disconnected" : "Connected", section_style());
        if (i == 3) draw_battery_meter(item_row, item_col, item_width, battery_value);
        ++shown;
      }
    } else if (cols >= 16 && pane_rows >= 23) {
      const int enabled = static_cast<int>(std::count(view.dashboard_widgets.begin(),
                                                       view.dashboard_widgets.end(), true));
      const int row = top + (pane_rows - std::max(1, enabled) * 6 + 1) / 2;
      const std::array<std::string, 4> values = {clock, weather_value, network_value, battery_value};
      const std::array<std::string, 4> details = {date, weather_detail, network_detail, battery_detail};
      const std::array<const char *, 4> names = {"CLOCK", "WEATHER", "NETWORK", "BATTERY"};
      int shown = 0;
      for (size_t i = 0; i < view.dashboard_widgets.size(); ++i) {
        if (!view.dashboard_widgets[i]) continue;
        const int item_row = row + shown * 6;
        draw_widget(item_row, left, cols, names[i], values[i], details[i]);
        if (i == 1 && !view.weather.location.empty())
          draw_text(item_row + 1, left + 3, cols - 6, view.weather.location, section_style());
        if (i == 2)
          draw_text(item_row + 1, left + 3, cols - 6,
                    network_value == "Offline" ? "Disconnected" : "Connected", section_style());
        if (i == 3) draw_battery_meter(item_row, left, cols, battery_value);
        ++shown;
      }
    } else {
      const std::array<std::string, 4> values = {clock, weather_value, network_value, battery_value};
      const std::array<const char *, 4> names = {"Clock", "Weather", "Network", "Battery"};
      int row = top;
      for (size_t i = 0; i < view.dashboard_widgets.size() && row < top + pane_rows; ++i)
        if (view.dashboard_widgets[i]) draw_field(row++, left, cols, names[i], values[i], body_style());
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
      draw_text(top, left, cols, "No notes yet.", muted_style());
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
    draw_text(top, left, cols, "Search the web", body_style());
  }

  void draw_settings(const ChromeView &view, int pane_rows, int left, int top, int cols) {
    if (pane_rows <= 0) return;
    const int bottom = top + pane_rows;
    const int inner_left = left + 2;
    const int inner_cols = std::max(0, cols - 2);
    auto section = [&](int row, const char *name, bool expanded, bool focused) {
      if (row >= bottom) return;
      const attr_t style = focused ? selected_style() : section_style();
      draw_widget_glyph(row, left, expanded ? L'▾' : L'▸', style);
      draw_text(row, left + 1, cols - 1, " [" + std::string(name) + "]", style);
    };
    auto option = [&](int row, const std::string &label, bool focused) {
      if (row >= bottom) return;
      draw_text(row, inner_left, inner_cols, label,
                focused ? selected_style() : body_style());
    };
    if (view.settings_selection == 2) {
      int row = top;
      constexpr std::array<const char *, 5> names = {
          "Network", "Audio", "Bluetooth", "Storage", "Peripherals"};
      for (size_t index = 0; index < names.size(); ++index) {
        const int content_bottom = bottom - 2 * static_cast<int>(names.size() - index - 1);
        section(row++, names[index], view.device_expanded[index],
                view.settings_item == index && !view.device_content_focused);
        if (view.device_expanded[index] && index == 0) {
          const std::string wifi = view.wifi.device.empty() ? "Unavailable" :
              !view.wifi.enabled ? "Off" :
              view.wifi.connected_ssid.empty() ? "Disconnected" : view.wifi.connected_ssid;
          option(row++, "WiFi: " + bracketed_value(wifi, inner_cols - 6),
                 view.settings_item == index && view.device_content_focused);
          const auto connected_lan = std::find_if(view.wifi.wired.begin(), view.wifi.wired.end(),
              [](const LanConnection &lan) { return lan.connected; });
          option(row++, "LAN: " + std::string(connected_lan == view.wifi.wired.end() ?
              "Disconnected" : "Connected"), false);
        } else if (view.device_expanded[index] && index == 1) {
          draw_audio(view, std::max(0, std::min(4, bottom - row)), inner_left, row, inner_cols);
          row += 4;
        } else if (view.device_expanded[index] && index == 2) {
          if (row < content_bottom)
            option(row++, "Power: " + std::string(view.bluetooth.available ?
                (view.bluetooth.powered ? "On" : "Off") : "Unavailable"), false);
          if (row < content_bottom) option(row++, "Status: " + view.bluetooth.status, false);
          const size_t connected = static_cast<size_t>(std::count_if(
              view.bluetooth.devices.begin(), view.bluetooth.devices.end(),
              [](const BluetoothDevice &device) { return device.connected; }));
          if (row < content_bottom) option(row++, "Connected devices:", false);
          if (connected == 0 && row < content_bottom - 1) option(row++, "  None", false);
          const int slots = std::max(0, content_bottom - row - 1);
          const bool truncated = connected > static_cast<size_t>(slots);
          const int visible = std::max(0, slots - (truncated ? 1 : 0));
          int shown = 0;
          for (const auto &device : view.bluetooth.devices) {
            if (!device.connected || shown >= visible) continue;
            option(row++, "  " + device.name, false);
            ++shown;
          }
          if (truncated && row < content_bottom - 1)
            option(row++, "  +" + std::to_string(connected - shown) + " more", false);
          if (row < content_bottom)
            option(row++, "[Add device]",
                   view.settings_item == index && view.device_content_focused);
        } else if (view.device_expanded[index] && index == 3) {
          const int visible = std::max(0, content_bottom - row);
          const size_t start = visible > 0 && view.storage_selection >= static_cast<size_t>(visible)
              ? view.storage_selection - static_cast<size_t>(visible) + 1 : 0;
          if (view.hardware.drives.empty() && visible > 0)
            option(row++, "No drives found", false);
          for (int i = 0; i < visible && start + static_cast<size_t>(i) < view.hardware.drives.size(); ++i) {
            const size_t selected = start + static_cast<size_t>(i);
            const auto &drive = view.hardware.drives[selected];
            const std::string label = drive.name + ": " +
                (drive.mountpoint.empty() ? storage_size(drive.size_bytes) + " [not mounted]" :
                    storage_size(drive.free_bytes) + "/" +
                    storage_size(drive.size_bytes) + " [" + drive.mountpoint + "]");
            option(row++, label, view.settings_item == index &&
                view.device_content_focused && view.storage_selection == selected);
          }
        } else if (view.device_expanded[index] && index == 4) {
          const int visible = std::max(0, content_bottom - row);
          const size_t start = visible > 0 && view.peripheral_selection >= static_cast<size_t>(visible)
              ? view.peripheral_selection - static_cast<size_t>(visible) + 1 : 0;
          if (view.hardware.usb_devices.empty() && visible > 0)
            option(row++, "No USB devices found", false);
          for (int i = 0; i < visible && start + static_cast<size_t>(i) < view.hardware.usb_devices.size(); ++i) {
            const size_t selected = start + static_cast<size_t>(i);
            option(row++, view.hardware.usb_devices[selected].name,
                   view.settings_item == index && view.device_content_focused &&
                   view.peripheral_selection == selected);
          }
        }
        ++row;
      }
      return;
    }
    if (view.settings_selection == 3) {
      draw_text(top, left, cols, "[Web]", section_style());
      for (size_t i = 0; i < kCookiePolicyNames.size(); ++i) {
        const int row = top + 1 + static_cast<int>(i);
        if (row >= top + pane_rows) break;
        const std::string label = std::string(view.cookie_policy == static_cast<CookiePolicy>(i) ?
            "[x] " : "[ ] ") + kCookiePolicyNames[i];
        option(row, label, view.cookie_selection == i);
      }
      const int links_row = top + 1 + static_cast<int>(kCookiePolicyNames.size());
      if (links_row < top + pane_rows) {
        const std::string label = std::string(view.show_web_header_links ? "[x] " : "[ ] ") +
            "Page header links";
        option(links_row, label, view.cookie_selection == kCookiePolicyNames.size());
      }
      if (links_row + 2 < top + pane_rows)
        draw_text(links_row + 2, inner_left, inner_cols,
                  "Block all may break some websites.", muted_style());
      return;
    }
    if (view.settings_selection == 0) {
      int row = top;
      section(row++, "Navigation", view.general_expanded[0], view.settings_item == 0);
      if (view.general_expanded[0])
        option(row++, std::string(view.vi_keys ? "[x] " : "[ ] ") +
                    "Vi motion (arrows always work)", view.settings_item == 1);
      ++row;
      section(row++, "Weather", view.general_expanded[1], view.settings_item == 2);
      if (view.general_expanded[1]) {
        option(row++, "City: " + bracketed_value(
                    view.weather.location.empty() ? std::string("Not set") :
                    view.weather.location, inner_cols - 6), view.settings_item == 3);
        if (row < bottom) draw_text(row++, inner_left, inner_cols,
                                   "Status: " + view.weather.status, muted_style());
      }
      return;
    }
    if (view.settings_selection == 1) {
      int row = top;
      section(row++, "Theme", view.interface_expanded[0], view.settings_item == 0);
      if (view.interface_expanded[0])
        for (size_t i = 0; i < kThemeNames.size(); ++i)
          option(row++, std::string(view.theme == static_cast<Theme>(i) ? "[x] " : "[ ] ") +
                    kThemeNames[i], view.settings_item == i + 1);
      ++row;
      section(row++, "Dashboard", view.interface_expanded[1], view.settings_item == 4);
      constexpr std::array<const char *, 4> names = {"Clock", "Weather", "Network", "Battery"};
      if (view.interface_expanded[1])
        for (size_t i = 0; i < names.size(); ++i)
          option(row++, std::string(view.dashboard_widgets[i] ? "[x] " : "[ ] ") + names[i],
                 view.settings_item == i + 5);
      ++row;
      section(row++, "Display", view.interface_expanded[2], view.settings_item == 9);
      if (view.interface_expanded[2]) {
        option(row++, std::string(view.unicode_glyphs ? "[x] " : "[ ] ") + "Unicode glyphs",
               view.settings_item == 10);
        option(row++, std::string(view.unicode_glyphs ? "[ ] " : "[x] ") + "ASCII glyphs",
               view.settings_item == 11);
      }
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
    if (top < bottom) draw_text(top, left, cols, "Status: " + view.audio.status,
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
      const bool audio_icon = ghostty_ && unicode_glyphs_ && cols >= 3;
      const int icon_width = audio_icon ? 3 : 0;
      const std::string label = index == 0 ? "Output: " : "Input: ";
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
      const std::string name = selected == devices.end() ?
          (devices.empty() ? "No device" : "Not selected") : selected->description;
      draw_text(row, left + icon_width, name_width, label +
          bracketed_value(name, name_width - static_cast<int>(label.size())), style);
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
          view.bluetooth.powered ? "No devices found" : "Bluetooth is off",
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
      const bool icon = ghostty_ && unicode_glyphs_ && cols >= 3;
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
          !view.wifi.enabled && !view.wifi.device.empty() ? "WiFi is off" :
          "No wired devices or WiFi networks found",
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
      const bool lock_icon = ghostty_ && unicode_glyphs_ && network.visible &&
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
  bool unicode_glyphs_ = true;
  UiLayout layout_ = ui_layout();
  bool ghostty_ = false;
  bool host_background_changed_ = false;
  Theme theme_ = Theme::Husky;
  std::map<std::pair<short, short>, short> pairs_;
  short next_pair_ = 5;
  bool menu_color_ = false;
};

}  // namespace cyberdeck
