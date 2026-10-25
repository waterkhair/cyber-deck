cmake_minimum_required(VERSION 3.16)
project(cyber_deck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CYBERDECK_ENABLE_LINUX_INTEGRATIONS
  "Build NetworkManager, PulseAudio, and BlueZ integrations" ON)
option(CYBERDECK_ENABLE_TERMUX_INTEGRATIONS
  "Build Termux:API information and music volume backends" OFF)
if(CYBERDECK_ENABLE_LINUX_INTEGRATIONS AND CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  message(FATAL_ERROR "Choose either Linux or Termux integrations")
endif()

set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND CYBERDECK_ENABLE_LINUX_INTEGRATIONS)
  pkg_check_modules(LIBNM QUIET IMPORTED_TARGET libnm>=1.24)
  pkg_check_modules(LIBPULSE QUIET IMPORTED_TARGET libpulse>=13)
  pkg_check_modules(GIO QUIET IMPORTED_TARGET gio-2.0>=2.56)
endif()
if(CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  if(NOT PkgConfig_FOUND)
    message(FATAL_ERROR "pkg-config is required for Termux integrations")
  endif()
  pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
endif()

find_path(VTERM_INCLUDE_DIR NAMES vterm.h)
find_library(VTERM_LIBRARY NAMES vterm)
if(NOT VTERM_INCLUDE_DIR OR NOT VTERM_LIBRARY)
  message(FATAL_ERROR "libvterm development headers and library are required")
endif()

include(CheckLibraryExists)
check_library_exists(util forkpty "" HAVE_FORKPTY_IN_UTIL)
if(HAVE_FORKPTY_IN_UTIL)
  set(PTY_LIBRARY util)
else()
  check_library_exists(c forkpty "" HAVE_FORKPTY_IN_C)
  if(NOT HAVE_FORKPTY_IN_C)
    message(FATAL_ERROR "forkpty is required (libutil or libc)")
  endif()
endif()

add_executable(cyber-deck
  src/main.cpp
  src/application.hpp
  src/cookies.hpp
  src/elinks.hpp
  src/dashboard.hpp
  src/input.hpp
  src/menu_art.hpp
  src/platform.hpp
  src/system.hpp
  src/terminal.hpp
  src/theme.hpp
  src/wave.cpp
  src/wave.hpp
  src/web.hpp)
# Termux's ncurses package includes wide-character support, but Android's
# default feature macros do not expose cchar_t and the wide curses functions.
target_compile_definitions(cyber-deck PRIVATE NCURSES_WIDECHAR=1)
target_include_directories(cyber-deck PRIVATE ${CURSES_INCLUDE_DIRS} ${VTERM_INCLUDE_DIR})
target_link_libraries(cyber-deck PRIVATE ${CURSES_LIBRARIES} ${VTERM_LIBRARY} ${PTY_LIBRARY} Threads::Threads)
if(CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  target_sources(cyber-deck PRIVATE src/wifi_termux.cpp src/termux_api.hpp)
  target_link_libraries(cyber-deck PRIVATE PkgConfig::JSONC)
elseif(CYBERDECK_ENABLE_LINUX_INTEGRATIONS AND LIBNM_FOUND)
  target_sources(cyber-deck PRIVATE src/wifi.cpp src/wifi.hpp)
  target_link_libraries(cyber-deck PRIVATE PkgConfig::LIBNM)
  target_compile_definitions(cyber-deck PRIVATE CYBERDECK_HAS_LIBNM=1)
else()
  target_sources(cyber-deck PRIVATE src/wifi_stub.cpp src/wifi.hpp)
  if(CYBERDECK_ENABLE_LINUX_INTEGRATIONS)
    message(STATUS "libnm not found: WiFi settings will be unavailable")
  endif()
endif()
if(CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  target_sources(cyber-deck PRIVATE src/audio_termux.cpp)
elseif(CYBERDECK_ENABLE_LINUX_INTEGRATIONS AND LIBPULSE_FOUND)
  target_sources(cyber-deck PRIVATE src/audio.cpp src/audio.hpp)
  target_link_libraries(cyber-deck PRIVATE PkgConfig::LIBPULSE)
else()
  target_sources(cyber-deck PRIVATE src/audio_stub.cpp src/audio.hpp)
  if(CYBERDECK_ENABLE_LINUX_INTEGRATIONS)
    message(STATUS "libpulse not found: Audio settings will be unavailable")
  endif()
endif()
if(CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  target_sources(cyber-deck PRIVATE src/bluetooth_termux.cpp)
elseif(CYBERDECK_ENABLE_LINUX_INTEGRATIONS AND GIO_FOUND)
  target_sources(cyber-deck PRIVATE src/bluetooth.cpp src/bluetooth.hpp)
  target_link_libraries(cyber-deck PRIVATE PkgConfig::GIO)
else()
  target_sources(cyber-deck PRIVATE src/bluetooth_stub.cpp src/bluetooth.hpp)
  if(CYBERDECK_ENABLE_LINUX_INTEGRATIONS)
    message(STATUS "gio-2.0 not found: Bluetooth settings will be unavailable")
  endif()
endif()
if(NOT CYBERDECK_ENABLE_LINUX_INTEGRATIONS AND NOT CYBERDECK_ENABLE_TERMUX_INTEGRATIONS)
  message(STATUS "Linux integrations disabled: WiFi, Audio, and Bluetooth controls will be unavailable")
endif()
target_compile_options(cyber-deck PRIVATE -Wall -Wextra -Wpedantic)
