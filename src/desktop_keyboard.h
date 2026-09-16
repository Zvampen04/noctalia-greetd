#pragma once
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"
#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

// Desktop-only transport. Login/lock keys still go directly to the in-process
// password field. This surface never acquires keyboard focus or grabs a gamepad.
class DesktopKeyboard {
public:
  explicit DesktopKeyboard(WaylandConnection& connection) {
    m_registry = wl_display_get_registry(connection.display());
    static const wl_registry_listener listener{
      .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& self = *static_cast<DesktopKeyboard*>(data);
        if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
          self.m_manager = static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
      },
      .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    wl_registry_add_listener(m_registry, &listener, this);
    if (wl_display_roundtrip(connection.display()) < 0 || !m_manager || !connection.seat())
      throw std::runtime_error("On Screen Keyboard requires the virtual-keyboard Wayland protocol");
    m_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m_manager, connection.seat());
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) throw std::runtime_error("On Screen Keyboard requires XDG_RUNTIME_DIR");
    m_path = std::string(runtime) + "/noctalia-desktop-keyboard.sock";
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (m_path.size() >= sizeof(address.sun_path)) throw std::runtime_error("Keyboard socket path is too long");
    std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
    m_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    // The owning systemd service removes a stale socket before startup. Never
    // unlink another running keyboard's socket from an uncoordinated launch.
    if (m_fd < 0 || bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
      throw std::runtime_error("On Screen Keyboard is already running or its control socket is unavailable");
    chmod(m_path.c_str(), 0600);
  }
  ~DesktopKeyboard() {
    if (m_fd >= 0) { close(m_fd); unlink(m_path.c_str()); }
    if (m_keyboard) zwp_virtual_keyboard_v1_destroy(m_keyboard);
    if (m_manager) zwp_virtual_keyboard_manager_v1_destroy(m_manager);
    if (m_registry) wl_registry_destroy(m_registry);
  }
  int fd() const { return m_fd; }
  void commands(const std::function<void(int,int)>& navigate, const std::function<void()>& activate) {
    char data[64];
    for (int n = 0; n < 32; ++n) {
      auto size = recv(m_fd, data, sizeof(data)-1, 0);
      if (size <= 0) break;
      data[size] = 0;
      int x = 0, y = 0; char extra;
      if (std::sscanf(data, "navigate %d %d %c", &x, &y, &extra) == 2 &&
          std::abs(x) + std::abs(y) == 1) navigate(x,y);
      else if (std::strcmp(data, "activate") == 0) activate();
    }
  }
  void send(uint32_t symbol, uint32_t) {
    unsigned code = KEY_A;
    switch (symbol) {
      case XKB_KEY_BackSpace: code=KEY_BACKSPACE; break;
      case XKB_KEY_Return: code=KEY_ENTER; break;
      case XKB_KEY_Left: code=KEY_LEFT; break;
      case XKB_KEY_Right: code=KEY_RIGHT; break;
    }
    // A one-level map carries the exact selected Unicode symbol, including
    // shifted and Swedish characters, independently of the hardware layout.
    char map[1024];
    std::snprintf(map, sizeof(map),
      "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; <OSK>=%u; }; "
      "xkb_types { type \"ONE_LEVEL\" { modifiers=None; level_name[Level1]=\"Any\"; }; }; "
      "xkb_compatibility {}; xkb_symbols { key <OSK> { type=\"ONE_LEVEL\", [ 0x%x ] }; }; };", code+8, symbol);
    int memory = memfd_create("desktop-keyboard-keymap", MFD_CLOEXEC);
    const auto size = std::strlen(map)+1;
    if (memory < 0) return;
    if (write(memory,map,size) != static_cast<ssize_t>(size)) { close(memory); return; }
    zwp_virtual_keyboard_v1_keymap(m_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, memory, size);
    close(memory);
    zwp_virtual_keyboard_v1_modifiers(m_keyboard,0,0,0,0);
    const auto now = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
    zwp_virtual_keyboard_v1_key(m_keyboard,now,code,WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(m_keyboard,now,code,WL_KEYBOARD_KEY_STATE_RELEASED);
  }
private:
  wl_registry* m_registry = nullptr;
  zwp_virtual_keyboard_manager_v1* m_manager = nullptr;
  zwp_virtual_keyboard_v1* m_keyboard = nullptr;
  int m_fd = -1;
  std::string m_path;
};
