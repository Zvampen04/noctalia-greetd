#pragma once

#include "ext-session-lock-v1-client-protocol.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"
#include <wayland-client.h>

// Only the Wayland surface role differs between login and locking. The scene,
// input dispatcher and keyboard are supplied by NativeGreeter in both modes.
class SessionLockSurface final : public Surface {
public:
  explicit SessionLockSurface(WaylandConnection& connection) : Surface(connection) {}
  ~SessionLockSurface() override {
    m_connection.unregisterSurface(m_surface);
    if (m_role) {
      if (!m_configured && !m_connection.findOutputByWl(m_output))
        wl_proxy_destroy(reinterpret_cast<wl_proxy*>(m_role));
      else
        ext_session_lock_surface_v1_destroy(m_role);
    }
  }

  bool initialize() override { return false; }
  void refreshOutputScale(const WaylandOutput& output) {
    updateOutputScale(output.scale, static_cast<uint32_t>(std::max(1, output.configuredScaleNumerator)));
  }
  bool initialize(ext_session_lock_v1* lock, const WaylandOutput& output) {
    m_output = output.output;
    setBufferScale(output.scale);
    setConfiguredScaleNumerator(output.configuredScaleNumerator);
    if (!lock || !createWlSurface()) return false;
    m_connection.registerSurfaceOutput(m_surface, m_output);
    m_role = ext_session_lock_v1_get_lock_surface(lock, m_surface, m_output);
    if (!m_role) return false;
    static const ext_session_lock_surface_v1_listener listener{
      .configure = [](void* data, ext_session_lock_surface_v1* role, uint32_t serial,
                      uint32_t width, uint32_t height) {
        auto& self = *static_cast<SessionLockSurface*>(data);
        self.m_configured = true;
        ext_session_lock_surface_v1_ack_configure(role, serial);
        self.onConfigure(width, height);
      },
    };
    ext_session_lock_surface_v1_add_listener(m_role, &listener, this);
    setRunning(true);
    return true;
  }
private:
  ext_session_lock_surface_v1* m_role = nullptr;
  wl_output* m_output = nullptr;
  bool m_configured = false;
};
