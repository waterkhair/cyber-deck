#pragma once

#include "cookies.hpp"
#include "theme.hpp"

#include <array>
#include <string>

namespace cyberdeck {

inline std::string elinks_configuration(Theme theme, CookiePolicy cookies,
                                       bool show_header_links) {
  const char *background = theme == Theme::Synthwave ? "#00005f" :
                           theme == Theme::Greenline ? "#080808" : "#101010";
  const char *foreground = theme == Theme::Synthwave ? "#ffd7ff" :
                           theme == Theme::Greenline ? "#d7ffd7" : "#eeeeee";
  const char *accent = theme == Theme::Synthwave ? "#00d7ff" :
                       theme == Theme::Greenline ? "#87ffaf" : "#ffffff";
  const char *highlight = theme == Theme::Synthwave ? "#af00af" :
                          theme == Theme::Greenline ? "#005f00" : "#555555";
  const char *highlight_text = theme == Theme::Greenline ? "#d7ffd7" : "#ffffff";
  std::string config;
  auto number = [&config](const std::string &name, int value) {
    config += "set " + name + " = " + std::to_string(value) + '\n';
  };
  auto color = [&config](const std::string &name, const char *value) {
    config += "set " + name + " = \"" + value + "\"\n";
  };
  auto pair = [&color](const std::string &name, const char *text, const char *back) {
    color(name + ".text", text);
    color(name + ".background", back);
  };

  number("terminal.xterm.colors", 1);
  number("terminal.xterm.transparency", 0);
  number("document.colors.use_document_colors", 0);
  number("document.html.link_display", show_header_links ? 2 : 0);
  color("document.colors.background", background);
  color("document.colors.text", foreground);
  color("document.colors.link", accent);
  color("document.colors.vlink", foreground);
  color("document.colors.image", accent);
  color("document.colors.bookmark", accent);
  number("ui.tabs.show_bar", 0);
  number("ui.show_menu_bar_always", 0);
  number("ui.show_title_bar", 0);
  number("ui.show_status_bar", 1);
  number("cookies.accept_policy", static_cast<int>(cookies));
  number("cookies.save", 0);
  number("cookies.max_age", 0);

  config += "bind \"main\" \"h\" = \"history-move-back\"\n"
            "bind \"main\" \"j\" = \"move-link-next\"\n"
            "bind \"main\" \"k\" = \"move-link-prev\"\n"
            "bind \"main\" \"l\" = \"link-follow\"\n"
            "bind \"menu\" \"h\" = \"left\"\n"
            "bind \"menu\" \"j\" = \"down\"\n"
            "bind \"menu\" \"k\" = \"up\"\n"
            "bind \"menu\" \"l\" = \"right\"\n";

  for (const char *name : {
           "mainmenu.normal", "mainmenu.hotkey.normal", "menu.normal",
           "menu.marked", "menu.hotkey.normal", "menu.frame",
           "dialog.generic", "dialog.frame", "dialog.title", "dialog.text",
           "dialog.checkbox", "dialog.checkbox-label", "dialog.field",
           "dialog.field-text", "dialog.meter", "dialog.shadow",
           "title.title-bar", "title.title-text", "status.status-bar",
           "status.status-text", "status.showip-text", "status.showmem-text",
           "desktop", "tabs.normal", "tabs.unvisited", "tabs.loading",
           "tabs.separator"})
    pair(std::string("ui.colors.color.") + name, foreground, background);
  for (const char *name : {
           "mainmenu.selected", "mainmenu.hotkey.selected", "menu.selected",
           "menu.hotkey.selected", "dialog.scrollbar-selected",
           "dialog.checkbox-selected", "dialog.button-selected",
           "dialog.button-shortcut-selected", "tabs.selected"})
    pair(std::string("ui.colors.color.") + name, highlight_text, highlight);
  for (const char *name : {
           "dialog.button", "dialog.button-shortcut", "dialog.scrollbar"})
    pair(std::string("ui.colors.color.") + name, foreground, highlight);
  pair("ui.colors.color.clipboard", foreground, background);
  pair("ui.colors.color.searched", background, accent);
  return config;
}

}  // namespace cyberdeck
