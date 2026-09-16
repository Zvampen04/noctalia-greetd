#pragma once

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"
#include <wayland-client.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Gamepads become ordinary Wayland pointers only for the lifetime of this
// authentication screen. No uinput device or background input daemon is needed.
class ControllerPointer {
  struct Pad {
    std::string path;
    int fd = -1;
    zwlr_virtual_pointer_v1* pointer = nullptr;
    input_absinfo x{}, y{};
    bool left = false, right = false, sentLeft = false, sentRight = false;
    bool waitingNeutral = true, dropped = false, dead = false;
    int hatX = 0, hatY = 0, previousX = 0, previousY = 0;
    bool dpadLeft = false, dpadRight = false, dpadUp = false, dpadDown = false;
    uint32_t nextRepeat = 0;
    bool rawLeft = false, consumedLeft = false;
    ~Pad() { if (pointer) zwlr_virtual_pointer_v1_destroy(pointer); if (fd >= 0) close(fd); }
    static double normalized(const input_absinfo& axis) {
      if (axis.maximum <= axis.minimum) return 0;
      const double value = 2.0 * (double(axis.value) - axis.minimum) / (double(axis.maximum) - axis.minimum) - 1.0;
      const double magnitude = std::clamp((std::abs(value) - 0.18) / 0.82, 0.0, 1.0);
      return std::copysign(magnitude * magnitude, value);
    }
    void refresh() {
      ioctl(fd, EVIOCGABS(ABS_RX), &x);
      ioctl(fd, EVIOCGABS(ABS_RY), &y);
      input_absinfo hat{};
      if (ioctl(fd, EVIOCGABS(ABS_HAT0X), &hat) >= 0) hatX = hat.value;
      if (ioctl(fd, EVIOCGABS(ABS_HAT0Y), &hat) >= 0) hatY = hat.value;
      std::array<unsigned char, (KEY_MAX + 8) / 8> keys{};
      if (ioctl(fd, EVIOCGKEY(keys.size()), keys.data()) >= 0) {
        const auto held = [&](unsigned code) { return (keys[code / 8] & (1 << (code % 8))) != 0; };
        left = held(BTN_SOUTH);
        right = held(BTN_EAST);
        dpadLeft = held(BTN_DPAD_LEFT); dpadRight = held(BTN_DPAD_RIGHT);
        dpadUp = held(BTN_DPAD_UP); dpadDown = held(BTN_DPAD_DOWN);
      }
    }
    void button(uint32_t now, uint32_t code, bool state, bool& sent) {
      if (sent == state) return;
      zwlr_virtual_pointer_v1_button(pointer, now, code, state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
      sent = state;
    }
    void release(uint32_t now) {
      button(now, BTN_LEFT, false, sentLeft);
      button(now, BTN_RIGHT, false, sentRight);
      zwlr_virtual_pointer_v1_frame(pointer);
    }
    void tick(uint32_t now, double dt, ControllerPointer& owner) {
      input_event event{};
      ssize_t bytes;
      while ((bytes = read(fd, &event, sizeof(event))) == sizeof(event)) {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
          dropped = waitingNeutral = true;
          release(now);
          rawLeft = consumedLeft = false;
          previousX = previousY = 0;
          nextRepeat = 0;
        } else if (dropped) {
          if (event.type == EV_SYN && event.code == SYN_REPORT) { refresh(); dropped = false; }
        } else if (event.type == EV_ABS && event.code == ABS_RX) x.value = event.value;
        else if (event.type == EV_ABS && event.code == ABS_RY) y.value = event.value;
        else if (event.type == EV_ABS && event.code == ABS_HAT0X) hatX = event.value;
        else if (event.type == EV_ABS && event.code == ABS_HAT0Y) hatY = event.value;
        else if (event.type == EV_KEY && event.code == BTN_DPAD_LEFT) dpadLeft = event.value != 0;
        else if (event.type == EV_KEY && event.code == BTN_DPAD_RIGHT) dpadRight = event.value != 0;
        else if (event.type == EV_KEY && event.code == BTN_DPAD_UP) dpadUp = event.value != 0;
        else if (event.type == EV_KEY && event.code == BTN_DPAD_DOWN) dpadDown = event.value != 0;
        else if (event.type == EV_KEY && event.code == BTN_SOUTH) left = event.value != 0;
        else if (event.type == EV_KEY && event.code == BTN_EAST) right = event.value != 0;
      }
      if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EINTR)) { release(now); dead = true; return; }
      if (dropped) return;
      const double dx = normalized(x), dy = normalized(y);
      const int navX = std::clamp(hatX + int(dpadRight) - int(dpadLeft), -1, 1);
      const int navY = std::clamp(hatY + int(dpadDown) - int(dpadUp), -1, 1);
      if (waitingNeutral) {
        if (!left && !right && !hatX && !hatY && !dpadLeft && !dpadRight && !dpadUp && !dpadDown && dx == 0 && dy == 0)
          waitingNeutral = false;
        return;
      }
      if (dx || dy) {
        if (owner.onCursor) owner.onCursor();
        zwlr_virtual_pointer_v1_motion(pointer, now, wl_fixed_from_double(dx * 1100.0 * dt), wl_fixed_from_double(dy * 1100.0 * dt));
      }
      if ((navX || navY) && (navX != previousX || navY != previousY || static_cast<int32_t>(now - nextRepeat) >= 0)) {
        if (owner.onNavigate) owner.onNavigate(navX, navY);
        nextRepeat = now + (navX != previousX || navY != previousY ? 400 : 85);
      }
      previousX = navX; previousY = navY;
      if (left && !rawLeft) consumedLeft = owner.onActivate && owner.onActivate();
      if (!consumedLeft) button(now, BTN_LEFT, left, sentLeft);
      if (!left) consumedLeft = false;
      rawLeft = left;
      button(now, BTN_RIGHT, right, sentRight);
      zwlr_virtual_pointer_v1_frame(pointer);
    }
  };
public:
  std::function<void(int, int)> onNavigate;
  std::function<bool()> onActivate;
  std::function<void()> onCursor;
  explicit ControllerPointer(WaylandConnection& connection) : m_connection(connection) {
    m_registry = wl_display_get_registry(connection.display());
    static const wl_registry_listener listener{
      .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto& self = *static_cast<ControllerPointer*>(data);
        if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
          self.m_manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name,
              &zwlr_virtual_pointer_manager_v1_interface, std::min(version, 2u)));
      },
      .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    wl_registry_add_listener(m_registry, &listener, this);
    wl_display_roundtrip(connection.display());
  }
  ~ControllerPointer() {
    stop();
    if (m_manager) zwlr_virtual_pointer_manager_v1_destroy(m_manager);
    if (m_registry) wl_registry_destroy(m_registry);
  }
  void stop() {
    for (auto& pad : m_pads) pad->release(timestamp());
    m_pads.clear();
  }
  bool active() const { return !m_pads.empty(); }
  void tick() {
    if (!m_manager) return;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::clamp(std::chrono::duration<double>(now - m_lastTick).count(), 0.0, 0.05);
    m_lastTick = now;
    for (auto& pad : m_pads) pad->tick(timestamp(), dt, *this);
    std::erase_if(m_pads, [](const auto& pad) { return pad->dead; });
    if (now < m_nextScan) return;
    m_nextScan = now + std::chrono::seconds(2);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
      const auto path = entry.path().string();
      if (!entry.path().filename().string().starts_with("event") || m_pads.size() >= 8) continue;
      if (std::any_of(m_pads.begin(), m_pads.end(), [&](const auto& pad) { return pad->path == path; })) continue;
      auto pad = std::make_unique<Pad>();
      pad->fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (pad->fd < 0) continue;
      std::array<unsigned char, (KEY_MAX + 8) / 8> keys{};
      if (ioctl(pad->fd, EVIOCGBIT(EV_KEY, keys.size()), keys.data()) < 0 ||
          !(keys[BTN_GAMEPAD / 8] & (1 << (BTN_GAMEPAD % 8))) ||
          ioctl(pad->fd, EVIOCGABS(ABS_RX), &pad->x) < 0 ||
          ioctl(pad->fd, EVIOCGABS(ABS_RY), &pad->y) < 0) continue;
      if (ioctl(pad->fd, EVIOCGRAB, 1) < 0) continue;
      pad->path = path;
      pad->refresh();
      pad->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m_manager, m_connection.seat());
      if (pad->pointer) m_pads.push_back(std::move(pad));
    }
  }
private:
  static uint32_t timestamp() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  WaylandConnection& m_connection;
  wl_registry* m_registry = nullptr;
  zwlr_virtual_pointer_manager_v1* m_manager = nullptr;
  std::vector<std::unique_ptr<Pad>> m_pads;
  std::chrono::steady_clock::time_point m_nextScan{}, m_lastTick = std::chrono::steady_clock::now();
};
