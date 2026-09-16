#include "auth/pam_authenticator.h"
#include "appearance.h"
#include "lock_widgets_scene.h"
#include "lock_widget_services.h"
#include "render/animation/animation_manager.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "session_lock_surface.h"
#include "on_screen_keyboard.h"
#include "desktop_keyboard.h"
#include "controller_pointer.h"
#include "session_lock_hint.h"
#include <sys/resource.h>
#if __has_include("core/input/key_symbols.h")
#include "core/input/key_symbols.h"
#else
#include "core/key_symbols.h"
#endif
#include "core/timer_manager.h"
#include "render/core/color.h"
#include "render/animation/motion_service.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lock_visual_layout.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "ksld-client-protocol.h"

#if __has_include(<json.hpp>)
#include <json.hpp>
#else
#include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client-core.h>

using json = nlohmann::json;

namespace {

template <typename Event>
bool pointerEventPressed(const Event& event) {
  if constexpr (requires { event.pressed; }) {
    return event.pressed;
  } else {
    return event.state != 0;
  }
}

template <typename SurfaceType, typename RenderContextType>
Renderer& layoutRenderer(SurfaceType& surface, RenderContextType& fallback) {
  if constexpr (requires { surface.renderTarget().renderer(); }) {
    return surface.renderTarget().renderer();
  } else {
    return fallback;
  }
}

template <typename Dispatcher, typename Event>
void dispatchPointerButton(Dispatcher& dispatcher, const Event& event) {
  const float x = static_cast<float>(event.sx);
  const float y = static_cast<float>(event.sy);
  const bool pressed = pointerEventPressed(event);
  if constexpr (requires {
                  dispatcher.pointerButton(x, y, event.button, pressed, event.serial, event.time, event.touch);
                }) {
    dispatcher.pointerButton(x, y, event.button, pressed, event.serial, event.time, event.touch);
  } else {
    dispatcher.pointerButton(x, y, event.button, pressed);
  }
}

struct Session {
  std::string id;
  std::string name;
  std::string desktop;
  std::vector<std::string> command;
};

struct Profile {
  std::string username;
  std::string name;
};

struct ProgramOptions {
  bool lock = false;
  bool keyboard = false;
  bool kscreenlocker = false;
  bool immediateLock = false;
  bool noLock = false;
  int graceTimeMs = 0;
  int ksldFd = -1;
};

std::string envOr(std::string_view key, std::string fallback = {}) {
  const std::string name(key);
  if (const char* value = std::getenv(name.c_str()); value != nullptr && value[0] != '\0') {
    return value;
  }
  return fallback;
}

float envFloat(std::string_view key, float fallback) {
  const std::string raw = envOr(key);
  if (raw.empty()) {
    return fallback;
  }

  char* end = nullptr;
  const float value = std::strtof(raw.c_str(), &end);
  if (end == raw.c_str() || *end != '\0') {
    return fallback;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

bool envBool(std::string_view key, bool fallback) {
  const auto raw = envOr(key);
  if (raw.empty()) {
    return fallback;
  }
  return raw == "1" || raw == "true" || raw == "yes" || raw == "on";
}

int parseIntArg(std::string_view value, std::string_view option) {
  std::string text(value);
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed < -1 || parsed > 1'000'000'000L) {
    throw std::runtime_error(std::format("invalid value for {}: {}", option, value));
  }
  return static_cast<int>(parsed);
}

ProgramOptions parseOptions(int argc, char** argv) {
  ProgramOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i] != nullptr ? argv[i] : "");
    if (arg == "--lock") {
      options.lock = true;
    } else if (arg == "--keyboard") {
      options.keyboard = true;
    } else if (arg == "--kscreenlocker") {
      options.kscreenlocker = true;
    } else if (arg == "--immediateLock") {
      options.immediateLock = true;
    } else if (arg == "--nolock") {
      options.noLock = true;
    } else if (arg == "--graceTime" || arg == "--ksldfd") {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::format("{} requires a value", arg));
      }
      const int value = parseIntArg(argv[++i], arg);
      if (arg == "--graceTime") {
        options.graceTimeMs = value;
      } else {
        options.ksldFd = value;
      }
    } else {
      throw std::runtime_error(std::format("unknown option: {}", arg));
    }
  }
  if (options.lock && (options.kscreenlocker || options.noLock))
    throw std::runtime_error("--lock cannot be combined with KScreenLocker or --nolock");
  if (options.keyboard && (options.lock || options.kscreenlocker))
    throw std::runtime_error("--keyboard cannot be combined with a lock mode");
  return options;
}

void secureClear(std::string& value) {
  volatile char* ptr = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) {
    ptr[i] = '\0';
  }
  value.clear();
}

std::vector<std::string> jsonStringArray(const json& value) {
  std::vector<std::string> result;
  for (const auto& item : value) {
    result.push_back(item.get<std::string>());
  }
  return result;
}

std::vector<Session> parseSessions() {
  std::vector<Session> sessions;
  const auto raw = envOr("LOCKSCREEN_GREETER_SESSIONS", "[]");
  for (const auto& item : json::parse(raw)) {
    sessions.push_back(Session{
        .id = item.at("id").get<std::string>(),
        .name = item.at("name").get<std::string>(),
        .desktop = item.at("desktop").get<std::string>(),
        .command = jsonStringArray(item.at("command")),
    });
  }
  return sessions;
}

std::vector<Profile> parseProfiles() {
  std::vector<Profile> profiles;
  const auto fallbackUser = envOr("LOCKSCREEN_GREETER_USER", "Zvampen04");
  const auto raw = envOr("LOCKSCREEN_GREETER_USERS");
  if (!raw.empty()) {
    for (const auto& item : json::parse(raw)) {
      const auto username = item.at("username").get<std::string>();
      profiles.push_back(Profile{
          .username = username,
          .name = item.value("name", username),
      });
    }
  }

  if (profiles.empty()) {
    profiles.push_back(Profile{
        .username = fallbackUser,
        .name = fallbackUser,
    });
  }
  return profiles;
}

std::vector<Profile> currentUserProfile() {
  std::string username = PamAuthenticator::currentUsername();
  if (username.empty()) {
    username = envOr("USER", envOr("LOGNAME", "user"));
  }
  return {Profile{.username = username, .name = username}};
}

std::vector<std::string> parseRootCommand() {
  return jsonStringArray(json::parse(envOr("LOCKSCREEN_GREETER_ROOT_COMMAND", R"(["bash","-l"])")));
}

void writeAll(int fd, const void* data, std::size_t size) {
  const auto* ptr = static_cast<const std::byte*>(data);
  while (size > 0) {
    const ssize_t written = ::write(fd, ptr, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
    }
    ptr += written;
    size -= static_cast<std::size_t>(written);
  }
}

void readAll(int fd, void* data, std::size_t size) {
  auto* ptr = static_cast<std::byte*>(data);
  while (size > 0) {
    const ssize_t got = ::read(fd, ptr, size);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
    }
    if (got == 0) {
      throw std::runtime_error("greetd closed the connection");
    }
    ptr += got;
    size -= static_cast<std::size_t>(got);
  }
}

class GreetdClient {
public:
  GreetdClient() {
    const auto sockPath = envOr("GREETD_SOCK");
    if (sockPath.empty()) {
      throw std::runtime_error("GREETD_SOCK is not set");
    }

    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) {
      throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof(addr.sun_path)) {
      throw std::runtime_error("GREETD_SOCK path is too long");
    }
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
    }
  }

  GreetdClient(const GreetdClient&) = delete;
  GreetdClient& operator=(const GreetdClient&) = delete;

  ~GreetdClient() {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
  }

  json request(const json& payload) {
    const auto encoded = payload.dump();
    const auto size = static_cast<std::uint32_t>(encoded.size());
    writeAll(m_fd, &size, sizeof(size));
    writeAll(m_fd, encoded.data(), encoded.size());

    std::uint32_t responseSize = 0;
    readAll(m_fd, &responseSize, sizeof(responseSize));
    std::string response(responseSize, '\0');
    readAll(m_fd, response.data(), response.size());
    return json::parse(response);
  }

  void cancel() {
    try {
      (void)request(json{{"type", "cancel_session"}});
    } catch (...) {
    }
  }

private:
  int m_fd = -1;
};

class KScreenLockerBridge {
public:
  KScreenLockerBridge() = default;
  KScreenLockerBridge(const KScreenLockerBridge&) = delete;
  KScreenLockerBridge& operator=(const KScreenLockerBridge&) = delete;

  ~KScreenLockerBridge() { cleanup(); }

  bool initialize(int fd) {
    if (fd < 0) {
      return true;
    }

    m_display = wl_display_connect_to_fd(fd);
    if (m_display == nullptr) {
      return false;
    }

    m_registry = wl_display_get_registry(m_display);
    if (m_registry == nullptr) {
      cleanup();
      return false;
    }

    if (wl_registry_add_listener(m_registry, &kRegistryListener, this) != 0) {
      cleanup();
      return false;
    }

    if (wl_display_roundtrip(m_display) < 0 || m_ksld == nullptr) {
      cleanup();
      return false;
    }

    return true;
  }

  void notifySurfaceVisible() {
    if (m_notified || m_ksld == nullptr || m_display == nullptr) {
      return;
    }
    org_kde_ksld_x11window(m_ksld, 0);
    wl_display_flush(m_display);
    m_notified = true;
  }

private:
  static void handleGlobal(
      void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version
  ) {
    auto* self = static_cast<KScreenLockerBridge*>(data);
    if (std::strcmp(interface, org_kde_ksld_interface.name) != 0 || self->m_ksld != nullptr) {
      return;
    }
    self->m_ksld = static_cast<org_kde_ksld*>(
        wl_registry_bind(registry, name, &org_kde_ksld_interface, std::min<std::uint32_t>(version, 1))
    );
  }

  static void handleGlobalRemove(void*, wl_registry*, std::uint32_t) {}

  void cleanup() {
    if (m_ksld != nullptr) {
      org_kde_ksld_destroy(m_ksld);
      m_ksld = nullptr;
    }
    if (m_registry != nullptr) {
      wl_registry_destroy(m_registry);
      m_registry = nullptr;
    }
    if (m_display != nullptr) {
      wl_display_disconnect(m_display);
      m_display = nullptr;
    }
  }

  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  org_kde_ksld* m_ksld = nullptr;
  bool m_notified = false;

  static const wl_registry_listener kRegistryListener;
};

const wl_registry_listener KScreenLockerBridge::kRegistryListener = {
    .global = &KScreenLockerBridge::handleGlobal,
    .global_remove = &KScreenLockerBridge::handleGlobalRemove,
};

std::string localClockText() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::array<char, 16> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%H:%M", &tm);
  return buffer.data();
}

bool parseColorWallpaperPath(std::string_view path, Color& out) {
  constexpr std::string_view prefix = "color:";
  if (!path.starts_with(prefix)) {
    return false;
  }
  return tryParseHexColor(path.substr(prefix.size()), out);
}

std::optional<Color> jsonColor(const json& object, std::string_view key) {
  auto it = object.find(std::string(key));
  if (it == object.end() || !it->is_string()) {
    return std::nullopt;
  }

  Color color{};
  if (!tryParseHexColor(it->get<std::string>(), color)) {
    return std::nullopt;
  }
  return color;
}

bool applyPaletteFromFile(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  try {
    std::ifstream in(path);
    if (!in) {
      return false;
    }

    json object = json::parse(in);
    if (!object.is_object()) {
      return false;
    }

    Palette next = palette;
    std::size_t applied = 0;
    auto assign = [&](std::string_view key, Color& target) {
      if (auto color = jsonColor(object, key)) {
        target = *color;
        ++applied;
      }
    };

    assign("primary", next.primary);
    assign("on_primary", next.onPrimary);
    assign("secondary", next.secondary);
    assign("on_secondary", next.onSecondary);
    assign("tertiary", next.tertiary);
    assign("on_tertiary", next.onTertiary);
    assign("error", next.error);
    assign("on_error", next.onError);
    assign("surface", next.surface);
    assign("on_surface", next.onSurface);
    assign("surface_variant", next.surfaceVariant);
    assign("on_surface_variant", next.onSurfaceVariant);
    assign("outline", next.outline);
    assign("shadow", next.shadow);
    assign("hover", next.hover);
    assign("on_hover", next.onHover);

    if (applied == 0) {
      return false;
    }

    setPalette(next);
    std::cerr << "noctalia-greetd: loaded palette from " << path << "\n";
    return true;
  } catch (const std::exception& error) {
    std::cerr << "noctalia-greetd: failed to load palette " << path << ": " << error.what() << "\n";
    return false;
  }
}

class NativeGreeter {
public:
  NativeGreeter(
      WaylandConnection& wayland, RenderContext& renderContext, SharedTextureCache& textureCache,
      std::vector<Profile> profiles, std::string wallpaper, std::vector<Session> sessions,
      std::vector<std::string> rootCommand, ProgramOptions options, KScreenLockerBridge* kscreenlockerBridge,
      LockWidgetServices& widgetServices
  )
      : m_wayland(wayland),
        m_widgetServices(widgetServices),
        m_renderContext(renderContext),
        m_textureCache(textureCache),
        m_profiles(std::move(profiles)),
        m_wallpaperPath(std::move(wallpaper)),
        m_sessions(std::move(sessions)),
        m_rootCommand(std::move(rootCommand)),
        m_options(options),
        m_kscreenlockerBridge(kscreenlockerBridge) {
    Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t) { return KeySymbol::isEnter(sym); });
    m_root.setAnimationManager(&m_animations);
    buildScene();
  }

  ~NativeGreeter() { secureClear(m_password); }

  void setAuthTarget(NativeGreeter* target) { m_authTarget = target; m_keyboard->setFullDisplay(target != nullptr); }
  std::function<void(uint32_t,uint32_t)> onDesktopKey;
  void setAuthenticationAllowed(bool allowed) { m_authenticationAllowed = allowed; }
  wl_output* output() const { return m_output; }
  bool ownsSurface(wl_surface* surface) const { return m_surface && m_surface->wlSurface() == surface; }
  OnScreenKeyboard& keyboard() { return *m_keyboard; }
  void detach() {
    // Drop service listeners before the last output surface disappears.
    m_lockWidgets.reset();
    m_surface.reset();
    m_output = nullptr;
  }
  void refresh() { if (m_surface) m_surface->requestUpdate(); }
  bool needsWidgetFrame() const {
    return m_surface && !m_options.keyboard && (m_animations.hasActive() || (m_lockWidgets && m_lockWidgets->needsFrameTick()));
  }
  void widgetFrame(float deltaMs) {
    if (!m_surface || !needsWidgetFrame()) return;
    m_renderContext.makeCurrent(m_surface->renderTarget());
    m_animations.tick(deltaMs);
    if (m_lockWidgets) m_lockWidgets->frameTick(deltaMs, layoutRenderer(*m_surface, m_renderContext));
    requestForDirtyScene();
  }
  void setWidgetAppearance(const greeter_appearance::LockWidgetLayout& layout) {
    m_widgetAppearance = layout;
    if (m_surface) m_surface->requestLayout();
  }
  void refreshAppearance() {
    // Restyle retained controls; do not replace inputs, focus, authentication or surfaces.
    const auto visit = [&](auto&& self, Node& node) -> void {
      if (auto* button = dynamic_cast<Button*>(&node)) {
        button->setFontSize(Style::fontSizeBody);
        button->setRadius(Style::scaledRadiusMd());
      } else if (auto* input = dynamic_cast<Input*>(&node)) {
        input->setFontSize(Style::fontSizeBody);
        input->setControlHeight(Style::controlHeight);
        input->setHorizontalPadding(Style::spaceSm);
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
        input->setFrameRadius(Style::scaledRadiusMd());
#endif
      }
      node.markLayoutDirty();
      node.markPaintDirty();
      for (const auto& child : node.children()) self(self, *child);
    };
    visit(visit, m_root);
    if (m_statusLabel) m_statusLabel->setFontSize(Style::fontSizeCaption);
    if (m_keyboard) m_keyboard->refreshAppearance();
    if (m_surface) m_surface->requestLayout();
  }
  void refreshOutputScale() {
    if (auto* lockSurface = dynamic_cast<SessionLockSurface*>(m_surface.get()))
      if (const auto* info = m_wayland.findOutputByWl(m_output)) lockSurface->refreshOutputScale(*info);
  }

  bool initialize(const WaylandOutput& info, ext_session_lock_v1* lock = nullptr) {
    detach();
    wl_output* output = info.output;
    m_output = output;
    const auto defaultWidth = static_cast<uint32_t>(info.effectiveLogicalWidth());
    const auto defaultHeight = static_cast<uint32_t>(info.effectiveLogicalHeight());
    if (lock) {
      m_surface = std::make_unique<SessionLockSurface>(m_wayland);
      m_initializeSurface = [this, lock, info]() {
        return static_cast<SessionLockSurface*>(m_surface.get())->initialize(lock, info);
      };
    } else if (m_wayland.hasLayerShell() && !envBool("LOCKSCREEN_GREETER_FORCE_TOPLEVEL", false)) {
      LayerSurfaceConfig config{
          .nameSpace = m_options.keyboard ? "noctalia-desktop-keyboard" : "noctalia-greetd",
          .layer = LayerShellLayer::Overlay,
          .anchor =
              (m_options.keyboard ? LayerShellAnchor::Bottom : LayerShellAnchor::Top | LayerShellAnchor::Bottom) | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = m_options.keyboard ? std::min(defaultHeight, 400U) : 0,
          .exclusiveZone = m_options.keyboard ? static_cast<int>(std::min(defaultHeight, 400U)) : -1,
          .keyboard = m_options.keyboard ? LayerShellKeyboard::None : LayerShellKeyboard::Exclusive,
          .defaultWidth = defaultWidth,
          .defaultHeight = defaultHeight,
      };
      auto layerSurface = std::make_unique<LayerSurface>(m_wayland, std::move(config));
      m_surface = std::move(layerSurface);
      m_initializeSurface = [this, output]() { return static_cast<LayerSurface*>(m_surface.get())->initialize(output); };
    } else {
      if (m_options.keyboard) throw std::runtime_error("On Screen Keyboard requires layer-shell");
      auto topLevelSurface = std::make_unique<ToplevelSurface>(m_wayland);
      m_surface = std::move(topLevelSurface);
      ToplevelSurfaceConfig config{
          .width = defaultWidth,
          .height = defaultHeight,
          .minWidth = defaultWidth,
          .minHeight = defaultHeight,
          .title = "Noctalia Greeter",
          .appId = "dev.noctalia.Greetd",
      };
      m_initializeSurface = [this, output, config = std::move(config)]() mutable {
        return static_cast<ToplevelSurface*>(m_surface.get())->initialize(output, std::move(config));
      };
    }

#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    // Login/lock must never publish or request a compositor backdrop.
    m_surface->setExternalMaterialAllowed(m_options.keyboard);
#endif
    m_surface->setRenderContext(&m_renderContext);
    m_surface->setSceneRoot(&m_root);
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
    m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });

    if (!m_initializeSurface || !m_initializeSurface()) {
      return false;
    }
    if (!m_options.lock && !m_options.keyboard)
      m_inputDispatcher.setTextInputContext(m_surface->wlSurface(), m_wayland.textInputService(), true);
    focusPasswordField();
    m_surface->requestUpdate();
    if (m_kscreenlockerBridge != nullptr) {
      m_kscreenlockerBridge->notifySurfaceVisible();
    }
    return true;
  }

  void handlePointerEvent(const PointerEvent& event) {
    if (m_surface == nullptr || !ownsPointerEvent(event)) {
      return;
    }

    if (event.type == PointerEvent::Type::Motion ||
        (event.type == PointerEvent::Type::Button && pointerEventPressed(event)))
      m_keyboard->clearSelection();
    if (!m_loggedPointerEvent) {
      std::cerr << "noctalia-greetd: pointer input received\n";
      m_loggedPointerEvent = true;
    }

    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_inputDispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      break;
    case PointerEvent::Type::Button:
      dispatchPointerButton(m_inputDispatcher, event);
      break;
    case PointerEvent::Type::Axis:
      if (handleSessionMenuAxisScroll(event.axisValue120, event.axisValue, event.axisDiscrete)) {
        break;
      }
      m_inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
      break;
    }
    requestForDirtyScene();
  }

  void handleKeyboardEvent(const KeyboardEvent& event) {
    if (!m_loggedKeyboardEvent && event.pressed) {
      std::cerr << "noctalia-greetd: keyboard input received\n";
      m_loggedKeyboardEvent = true;
    }

    if (m_passwordField != nullptr && m_inputDispatcher.focusedArea() != m_passwordField->inputArea()) {
      focusPasswordField();
    }
    m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    requestForDirtyScene();
  }

  [[nodiscard]] bool running() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }

private:
  [[nodiscard]] bool ownsPointerEvent(const PointerEvent& event) const {
    if (m_surface == nullptr) {
      return false;
    }

    wl_surface* ownSurface = m_surface->wlSurface();
    if (event.surface == ownSurface) {
      return true;
    }

    if (event.surface != nullptr) {
      return false;
    }

    // wl_pointer.motion events do not carry a surface. WaylandSeat keeps the
    // surface from the last enter event, so use that for motion/axis filtering.
    return m_wayland.lastPointerSurface() == ownSurface;
  }

  void buildScene() {
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_root.setMaterialSurface(m_options.keyboard ? "osk" : (m_options.lock || m_options.kscreenlocker) ? "lock" : "greeter");
#endif
    if (m_options.keyboard) {
      m_keyboard = std::make_unique<OnScreenKeyboard>(m_root,
        [this](uint32_t symbol, uint32_t unicode) { if (onDesktopKey) onDesktopKey(symbol,unicode); },
        [this] { if (m_keyboard && !m_keyboard->visible()) m_done = true; if (m_surface) m_surface->requestLayout(); });
      m_keyboard->setDesktopMaterial(true);
      m_keyboard->setFullDisplay(true);
      m_keyboard->setVisible(true);
      m_inputDispatcher.setSceneRoot(&m_root);
      m_inputDispatcher.setCursorShapeCallback([this](uint32_t serial, uint32_t shape) { m_wayland.setCursorShape(serial,shape); });
      return;
    }
    auto wallpaper = std::make_unique<WallpaperNode>();
    m_wallpaper = static_cast<WallpaperNode*>(m_root.addChild(std::move(wallpaper)));
    m_wallpaper->setZIndex(0);

    m_root.addChild(ui::box({
        .out = &m_tintOverlay,
        .visible = false,
        .configure = [](Box& box) { box.setZIndex(1); },
    }));

    m_root.addChild(ui::box({
        .out = &m_backdrop,
        .configure = [](Box& box) { box.setZIndex(-1); },
    }));

    m_root.addChild(ui::label({
        .out = &m_clock,
        .color = colorSpecFromRole(ColorRole::Primary),
    }));

    m_root.addChild(ui::box({.out = &m_loginPanel}));
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_loginPanel->setMaterialIdentity("surface", "panel");
    m_loginPanel->setMaterialBackdrop(MaterialBackdrop::Local);
#endif
    m_root.addChild(ui::box({
        .out = &m_passwordFocusRing,
        .visible = false,
        .configure = [](Box& box) {
          box.setHitTestVisible(false);
          box.setZIndex(12);
        },
    }));
    m_root.addChild(ui::input({
        .out = &m_passwordField,
        .placeholder = "Password",
        .passwordMode = true,
        .onChange = [this](const std::string& value) { m_password = value; },
        .onSubmit = [this](const std::string&) { login(); },
    }));
    m_root.addChild(ui::button({
        .out = &m_loginButton,
        .text = "",
        .glyph = "check",
        .glyphSize = 16.0f,
        .variant = ButtonVariant::Primary,
        .onClick = [this]() { login(); },
    }));

    m_root.addChild(ui::label({
        .out = &m_statusLabel,
        .fontSize = Style::fontSizeCaption,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .textAlign = TextAlign::Center,
        .visible = false,
        .configure = [](Label& label) { label.setZIndex(13); },
    }));

    m_root.addChild(ui::button({
        .out = &m_rootButton,
        .text = "Root login",
        .fontSize = Style::fontSizeBody,
        .variant = ButtonVariant::Outline,
        .onClick = [this]() { toggleRootMode(); },
    }));

    m_profileButtonText = m_profiles.empty() ? "User" : m_profiles.front().name;
    m_root.addChild(ui::button({
        .out = &m_profileButton,
        .text = m_profileButtonText,
        .glyph = "chevron-up",
        .glyphSize = 14.0f,
        .variant = ButtonVariant::Outline,
        .onClick = [this]() { toggleProfileMenu(); },
    }));

    m_root.addChild(ui::box({
        .out = &m_profileMenu,
        .visible = false,
        .configure = [](Box& box) { box.setZIndex(30); },
    }));
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_profileMenu->setMaterialIdentity("surface", "panel");
    m_profileMenu->setMaterialBackdrop(MaterialBackdrop::Local);
#endif
    for (std::size_t i = 0; i < m_profiles.size(); ++i) {
      Button* item = nullptr;
      m_root.addChild(ui::button({
          .out = &item,
          .text = m_profiles[i].name,
          .fontSize = Style::fontSizeBody,
          .variant = ButtonVariant::Ghost,
          .visible = false,
          .onClick = [this, i]() { selectProfile(i); },
          .configure = [](Button& button) { button.setZIndex(31); },
      }));
      m_profileItems.push_back(item);
    }

    std::vector<std::string> sessionNames;
    for (const auto& session : m_sessions) {
      sessionNames.push_back(session.name);
    }
    m_sessionButtonText = sessionNames.empty() ? "Session" : sessionNames.front();
    m_root.addChild(ui::button({
        .out = &m_sessionButton,
        .text = m_sessionButtonText,
        .glyph = "chevron-up",
        .glyphSize = 14.0f,
        .variant = ButtonVariant::Outline,
        .onClick = [this]() { toggleSessionMenu(); },
    }));

    m_root.addChild(ui::box({
        .out = &m_sessionMenu,
        .visible = false,
        .configure = [](Box& box) { box.setZIndex(20); },
    }));
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_sessionMenu->setMaterialIdentity("surface", "panel");
    m_sessionMenu->setMaterialBackdrop(MaterialBackdrop::Local);
#endif
    for (std::size_t i = 0; i < m_sessions.size(); ++i) {
      Button* item = nullptr;
      m_root.addChild(ui::button({
          .out = &item,
          .text = m_sessions[i].name,
          .fontSize = Style::fontSizeBody,
          .variant = ButtonVariant::Ghost,
          .visible = false,
          .onClick = [this, i]() { selectSession(i); },
          .configure = [](Button& button) { button.setZIndex(21); },
      }));
      m_sessionItems.push_back(item);
    }

    m_keyboard = std::make_unique<OnScreenKeyboard>(m_root,
      [this](uint32_t sym, uint32_t unicode) {
        auto& target = m_authTarget ? *m_authTarget : *this;
        if (target.m_authBusy) return;
        target.focusPasswordField();
        target.m_inputDispatcher.keyEvent(sym, unicode, 0, true, false);
        target.m_inputDispatcher.keyEvent(sym, unicode, 0, false, false);
        target.requestForDirtyScene();
      }, [this] { refresh(); });
    m_inputDispatcher.setSceneRoot(&m_root);
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, shape);
    });
  }

  void prepareFrame(bool needsUpdate, bool needsLayout) {
    if (m_surface == nullptr || m_surface->width() == 0 || m_surface->height() == 0) {
      return;
    }

    m_renderContext.makeCurrent(m_surface->renderTarget());
    if (needsUpdate) {
      updateClockText();
      if (m_passwordField != nullptr && m_passwordField->value() != m_password) {
        m_passwordField->setValue(m_password);
      }
    }
    if (needsUpdate || needsLayout) {
      layoutScene(m_surface->width(), m_surface->height());
    }
  }

  void layoutScene(std::uint32_t width, std::uint32_t height) {
    if (m_options.keyboard) {
      m_root.setSize(width,height);
      m_keyboard->layout(layoutRenderer(*m_surface, m_renderContext),width,height);
#if defined(NOCTALIA_HAS_SURFACE_MATERIALS) && !defined(NOCTALIA_GREETER_FULL_APPEARANCE)
      // Only the desktop keyboard samples a compositor backdrop. Login and
      // locking keep their own wallpaper scene and never sample the session.
      if (m_keyboard->visible() && Style::surfaceMaterial() == Style::SurfaceMaterialMode::LiquidGlass) {
        const auto box = m_keyboard->panelRect();
        m_surface->setBlurRegion(Surface::tessellateRoundedRect(
            static_cast<int>(box.x), static_cast<int>(box.y),
            static_cast<int>(box.width), static_cast<int>(box.height), Style::scaledRadius(18.0F)));
      } else {
        m_surface->clearBlurRegion();
      }
#endif
      return;
    }
    applyWallpaperTexture();
    // The shared image cache can change the current EGL context while loading.
    m_renderContext.makeCurrent(m_surface->renderTarget());

    const float sw = static_cast<float>(width);
    const float sh = static_cast<float>(height) - m_keyboard->reservation(height);
    const auto* outputInfo = m_wayland.findOutputByWl(m_output);
    const std::string widgetOutput = outputInfo ? desktop_widgets::outputKey(*outputInfo) : std::string{};
    std::optional<lockscreen::LoginPanelPlacement> loginPlacement;
    for (const auto& widget : m_widgetAppearance.widgets) {
      if (widget.type != "login_box" || !widget.enabled
          || (widget.output.empty() ? m_authTarget != nullptr : widget.output != widgetOutput)) continue;
      loginPlacement = lockscreen::loginPanelPlacement(widget,sw,static_cast<float>(height));
      break;
    }
    const auto visual = lockscreen::layoutLockVisual(lockscreen::LockVisualLayoutParams{
        .renderer = layoutRenderer(*m_surface, m_renderContext),
        .root = m_root,
        .wallpaper = *m_wallpaper,
        .backdrop = *m_backdrop,
        .tintOverlay = m_tintOverlay,
        .clock = *m_clock,
        .loginPanel = *m_loginPanel,
        .passwordField = *m_passwordField,
        .loginButton = *m_loginButton,
        .width = width,
        .height = height,
        .bottomReservation = m_keyboard->reservation(height),
        .wallpaperFillMode = m_wallpaperFillMode,
        .wallpaperFillColor = m_wallpaperFillColor,
        .tintIntensity = m_wallpaperTintIntensity,
        .clockShadowEnabled = m_clockShadowEnabled,
        .loginPlacement = loginPlacement,
    });

    // Decorations can never cover the retained authentication controls.
    m_loginPanel->setZIndex(5);
    m_passwordField->setZIndex(6);
    m_loginButton->setZIndex(6);
    m_statusLabel->setZIndex(6);
    if (!m_lockWidgets) m_lockWidgets = std::make_unique<LockWidgetsScene>(
        m_root, m_widgetServices.runtime(), LockWidgetsScene::Callbacks{
          .update = [this] { refresh(); },
          .layout = [this] { if (m_surface) m_surface->requestLayout(); },
          .redraw = [this] { if (m_surface) m_surface->requestRedraw(); },
          // The main loop observes needsFrameTick after service/timer dispatch.
          // A scheduling request alone must not repaint every output.
          .frame = [] {},
        }, &m_animations);
    m_lockWidgets->setServices(m_widgetServices.runtime());
    m_lockWidgets->sync(m_widgetAppearance, widgetOutput, !m_authTarget,
        layoutRenderer(*m_surface,m_renderContext), sw, static_cast<float>(height));
    m_clock->setVisible(!m_lockWidgets->hasClock());

    m_keyboard->layout(layoutRenderer(*m_surface, m_renderContext), sw, height);
    if (m_authTarget) {
      hideGreeterControls();
      m_loginPanel->setVisible(false);
      m_passwordField->setVisible(false);
      m_loginButton->setVisible(false);
      m_passwordFocusRing->setVisible(false);
      m_statusLabel->setVisible(false);
      return;
    }
    layoutPasswordFocusRing();
    layoutStatusLabel(visual);

    if (m_options.kscreenlocker || m_options.lock) {
      hideGreeterControls();
      return;
    }

    const float extraButtonH = Style::controlHeight + Style::spaceXs;
    const float extraBottom = 32.0f;
    const float edgeInset = 32.0f;
    const float buttonGap = 8.0f;
    const float sessionButtonW = std::min(220.0f, (sw - 80.0f) / 3.0f);
    const float profileButtonW = sessionButtonW;
    const float buttonY = sh - extraBottom - extraButtonH;
    const float profileX = sw - edgeInset - profileButtonW;
    const float sessionX = profileX - buttonGap - sessionButtonW;

    m_rootButton->setText(m_rootMode ? "User login" : "Root login");
    m_rootButton->setSize(std::min(128.0f, profileButtonW), extraButtonH);
    m_rootButton->setPosition(edgeInset, buttonY);
    m_rootButton->layout(layoutRenderer(*m_surface, m_renderContext));

    m_profileButton->setText(m_profileButtonText);
    m_profileButton->setEnabled(!m_rootMode);
    m_profileButton->setSize(profileButtonW, extraButtonH);
    m_profileButton->setPosition(profileX, buttonY);
    m_profileButton->layout(layoutRenderer(*m_surface, m_renderContext));

    m_sessionButton->setText(m_sessionButtonText);
    m_sessionButton->setEnabled(!m_rootMode);
    m_sessionButton->setSize(sessionButtonW, extraButtonH);
    m_sessionButton->setPosition(sessionX, buttonY);
    m_sessionButton->layout(layoutRenderer(*m_surface, m_renderContext));

    layoutSessionMenu(sessionX, buttonY, sessionButtonW);
    layoutProfileMenu(profileX, buttonY, profileButtonW);
  }

  void layoutStatusLabel(const lockscreen::LockVisualLayoutResult& visual) {
    if (m_statusLabel == nullptr) {
      return;
    }
    const bool visible = !m_statusText.empty();
    m_statusLabel->setVisible(visible);
    if (!visible) {
      return;
    }
    m_statusLabel->setText(m_statusText);
    m_statusLabel->setMaxWidth(std::max(120.0f, visual.panelWidth - Style::spaceLg * 2.0f));
    m_statusLabel->layout(layoutRenderer(*m_surface, m_renderContext));
    const float labelX = visual.panelX + std::round((visual.panelWidth - m_statusLabel->width()) * 0.5f);
    const float labelY = visual.panelY - m_statusLabel->height() - Style::spaceSm;
    m_statusLabel->setPosition(labelX, labelY);
  }

  void hideGreeterControls() {
    if (m_rootButton != nullptr) {
      m_rootButton->setVisible(false);
    }
    if (m_profileButton != nullptr) {
      m_profileButton->setVisible(false);
    }
    if (m_sessionButton != nullptr) {
      m_sessionButton->setVisible(false);
    }
    if (m_profileMenu != nullptr) {
      m_profileMenu->setVisible(false);
    }
    if (m_sessionMenu != nullptr) {
      m_sessionMenu->setVisible(false);
    }
    for (Button* item : m_profileItems) {
      item->setVisible(false);
    }
    for (Button* item : m_sessionItems) {
      item->setVisible(false);
    }
  }

  void layoutSessionMenu(float menuX, float buttonY, float menuWidth) {
    const float itemHeight = 34.0f;
    const float menuPad = 6.0f;
    const float menuGap = 4.0f;
    const std::size_t visibleSessionItems = std::min<std::size_t>(m_sessionItems.size(), kMaxSessionMenuItems);
    const auto maxSessionOffset = sessionMenuMaxOffset();
    if (m_sessionScrollOffset > maxSessionOffset) {
      m_sessionScrollOffset = maxSessionOffset;
    }
    const float menuHeight = menuPad * 2.0f
        + static_cast<float>(visibleSessionItems) * itemHeight
        + std::max(0.0f, static_cast<float>(visibleSessionItems - 1)) * menuGap;
    const float menuY = std::max(Style::spaceLg, buttonY - Style::spaceSm - menuHeight);

    m_sessionMenu->setVisible(m_sessionMenuOpen && !m_rootMode);
    m_sessionMenu->setPosition(menuX, menuY);
    m_sessionMenu->setSize(menuWidth, menuHeight);
    m_sessionMenu->setStyle(RoundedRectStyle{
        .fill = colorForRole(ColorRole::Surface),
        .border = colorForRole(ColorRole::Outline, 0.95f),
        .fillMode = FillMode::Solid,
        .radius = Style::scaledRadiusXl(),
        .softness = 1.0f,
        .borderWidth = Style::popupBordersEnabled() ? Style::borderWidth : 0.0F,
    });

    for (std::size_t i = 0; i < m_sessionItems.size(); ++i) {
      auto* item = m_sessionItems[i];
      const bool itemInWindow =
          i >= m_sessionScrollOffset && i < m_sessionScrollOffset + visibleSessionItems;
      item->setVisible(m_sessionMenuOpen && !m_rootMode && itemInWindow);
      if (!itemInWindow) {
        continue;
      }
      const std::size_t localIndex = i - m_sessionScrollOffset;
      item->setSize(menuWidth - menuPad * 2.0f, itemHeight);
      item->setPosition(menuX + menuPad, menuY + menuPad + static_cast<float>(localIndex) * (itemHeight + menuGap));
      item->layout(layoutRenderer(*m_surface, m_renderContext));
    }
  }

  bool handleSessionMenuAxisScroll(int axisValue120, float axisValue, std::int32_t axisDiscrete) {
    if (!m_sessionMenuOpen || m_rootMode || m_sessionItems.size() <= kMaxSessionMenuItems) {
      return false;
    }

    int deltaSteps = 0;
    if (axisDiscrete != 0) {
      deltaSteps = axisDiscrete;
    } else if (axisValue120 != 0) {
      deltaSteps = axisValue120 / 120;
      if (deltaSteps == 0) {
        deltaSteps = axisValue120 > 0 ? 1 : -1;
      }
    } else {
      const auto delta = axisValue * 120.0f;
      if (delta == 0.0f) {
        return false;
      }
      deltaSteps = delta > 0.0f ? 1 : -1;
    }

    if (deltaSteps == 0) {
      return false;
    }

    const bool down = deltaSteps < 0;
    bool scrolled = false;
    for (int i = 0; i < std::abs(deltaSteps); ++i) {
      const bool moved = down ? scrollSessionMenu(+1) : scrollSessionMenu(-1);
      scrolled = scrolled || moved;
      if (!moved) {
        break;
      }
    }
    return scrolled;
  }

  bool scrollSessionMenu(int direction) {
    if (m_sessionItems.size() <= kMaxSessionMenuItems) {
      return false;
    }

    const auto maxOffset = sessionMenuMaxOffset();
    if (direction < 0 && m_sessionScrollOffset > 0) {
      --m_sessionScrollOffset;
      if (m_surface != nullptr) {
        m_surface->requestLayout();
      }
      return true;
    }
    if (direction > 0 && m_sessionScrollOffset < maxOffset) {
      ++m_sessionScrollOffset;
      if (m_surface != nullptr) {
        m_surface->requestLayout();
      }
      return true;
    }
    return false;
  }

  std::size_t sessionMenuMaxOffset() const {
    if (m_sessionItems.size() <= kMaxSessionMenuItems) {
      return 0;
    }
    return m_sessionItems.size() - kMaxSessionMenuItems;
  }

  void layoutProfileMenu(float menuX, float buttonY, float menuWidth) {
    const float itemHeight = 34.0f;
    const float menuPad = 6.0f;
    const float menuGap = 4.0f;
    const float menuHeight = menuPad * 2.0f
        + static_cast<float>(m_profileItems.size()) * itemHeight
        + std::max(0.0f, static_cast<float>(m_profileItems.size() - 1)) * menuGap;
    const float menuY = std::max(Style::spaceLg, buttonY - Style::spaceSm - menuHeight);

    m_profileMenu->setVisible(m_profileMenuOpen && !m_rootMode);
    m_profileMenu->setPosition(menuX, menuY);
    m_profileMenu->setSize(menuWidth, menuHeight);
    m_profileMenu->setStyle(RoundedRectStyle{
        .fill = colorForRole(ColorRole::Surface),
        .border = colorForRole(ColorRole::Outline, 0.95f),
        .fillMode = FillMode::Solid,
        .radius = Style::scaledRadiusXl(),
        .softness = 1.0f,
        .borderWidth = Style::popupBordersEnabled() ? Style::borderWidth : 0.0F,
    });

    for (std::size_t i = 0; i < m_profileItems.size(); ++i) {
      auto* item = m_profileItems[i];
      item->setVisible(m_profileMenuOpen && !m_rootMode);
      item->setSize(menuWidth - menuPad * 2.0f, itemHeight);
      item->setPosition(menuX + menuPad, menuY + menuPad + static_cast<float>(i) * (itemHeight + menuGap));
      item->layout(layoutRenderer(*m_surface, m_renderContext));
    }
  }

  void layoutPasswordFocusRing() {
    if (m_passwordFocusRing == nullptr || m_passwordField == nullptr || m_passwordField->inputArea() == nullptr) {
      return;
    }

    const bool focused = m_passwordField->inputArea()->focused();
    m_passwordFocusRing->setVisible(focused && !m_authBusy);
    if (!focused || m_authBusy) {
      return;
    }

    constexpr float inset = 4.0f;
    m_passwordFocusRing->setPosition(m_passwordField->x() - inset, m_passwordField->y() - inset);
    m_passwordFocusRing->setSize(m_passwordField->width() + inset * 2.0f, m_passwordField->height() + inset * 2.0f);
    m_passwordFocusRing->setStyle(RoundedRectStyle{
        .fill = rgba(0.0f, 0.0f, 0.0f, 0.0f),
        .border = colorForRole(ColorRole::Primary, 0.95f),
        .fillMode = FillMode::Solid,
        .radius = Style::scaledRadiusLg(),
        .softness = 1.0f,
        .borderWidth = Style::borderWidth * 2.0f,
    });
  }

  void applyWallpaperTexture() {
    if (m_wallpaperApplied) {
      return;
    }

    Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
    if (m_wallpaperPath.empty() || parseColorWallpaperPath(m_wallpaperPath, color)) {
      m_wallpaper->setSources(
          WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0f, 0.0f, 0.0f, 1.0f), 0.0f,
          0.0f, 0.0f, 0.0f
      );
    } else if (!m_wallpaperPath.empty()) {
      m_wallpaperTexture = m_textureCache.acquire(m_wallpaperPath);
      TextureHandle textureToDisplay = m_wallpaperTexture;
      if (m_wallpaperTexture.id != 0 && m_wallpaperBlurIntensity > 0.0f) {
        m_renderContext.makeCurrent(m_surface->renderTarget());
        static constexpr int kBlurRounds = 3;
        const float blurRadius = m_wallpaperBlurIntensity * 40.0f;
        m_blurredWallpaperTexture = m_wallpaperBlurCache.get(
            m_renderContext.backend(), m_wallpaperTexture, static_cast<std::uint32_t>(m_wallpaperTexture.width),
            static_cast<std::uint32_t>(m_wallpaperTexture.height), blurRadius, kBlurRounds
        );
        if (m_blurredWallpaperTexture.id != 0) {
          textureToDisplay = m_blurredWallpaperTexture;
        }
      }
      m_wallpaper->setTextures(
          textureToDisplay.id, {}, static_cast<float>(textureToDisplay.width),
          static_cast<float>(textureToDisplay.height), 0.0f, 0.0f
      );
    }
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
    m_wallpaperApplied = true;
  }

  void updateClockText() {
    const auto text = localClockText();
    if (m_clock != nullptr && m_clock->text() != text) {
      m_clock->setText(text);
    }
  }

  void requestForDirtyScene() {
    if (m_surface == nullptr) {
      return;
    }
    if (m_root.layoutDirty()) {
      m_surface->requestLayout();
    } else if (m_root.paintDirty()) {
      m_surface->requestRedraw();
    }
  }

  void focusPasswordField() {
    if (m_passwordField != nullptr) {
      m_inputDispatcher.setFocus(m_passwordField->inputArea());
    }
  }

  void setStatus(std::string text) {
    m_statusText = std::move(text);
    if (m_surface != nullptr) {
      m_surface->requestUpdate();
    }
  }

  void toggleRootMode() {
    if (m_authBusy) {
      return;
    }
    m_rootMode = !m_rootMode;
    m_sessionMenuOpen = false;
    m_profileMenuOpen = false;
    m_password.clear();
    setStatus("");
    focusPasswordField();
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void toggleSessionMenu() {
    if (m_authBusy || m_rootMode) {
      return;
    }
    if (!m_sessionMenuOpen) {
      m_sessionScrollOffset = 0;
    }
    m_sessionMenuOpen = !m_sessionMenuOpen;
    m_profileMenuOpen = false;
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void toggleProfileMenu() {
    if (m_authBusy || m_rootMode) {
      return;
    }
    m_profileMenuOpen = !m_profileMenuOpen;
    m_sessionMenuOpen = false;
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void selectSession(std::size_t index) {
    if (index >= m_sessions.size()) {
      return;
    }
    m_selectedSessionIndex = index;
    m_sessionButtonText = m_sessions[index].name;
    m_sessionMenuOpen = false;
    focusPasswordField();
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void selectProfile(std::size_t index) {
    if (index >= m_profiles.size()) {
      return;
    }
    m_selectedProfileIndex = index;
    m_profileButtonText = m_profiles[index].name;
    m_profileMenuOpen = false;
    m_password.clear();
    setStatus("");
    focusPasswordField();
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  Session selectedSession() const {
    if (m_selectedSessionIndex < m_sessions.size()) {
      return m_sessions[m_selectedSessionIndex];
    }
    throw std::runtime_error("No Wayland sessions configured");
  }

  Profile selectedProfile() const {
    if (m_selectedProfileIndex < m_profiles.size()) {
      return m_profiles[m_selectedProfileIndex];
    }
    throw std::runtime_error("No user profiles configured");
  }

  void setBusy(bool busy) {
    m_authBusy = busy;
    m_passwordField->setEnabled(!busy);
    m_loginButton->setEnabled(!busy);
    m_rootButton->setEnabled(!busy);
    m_profileButton->setEnabled(!busy && !m_rootMode);
    m_sessionButton->setEnabled(!busy && !m_rootMode);
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void login() {
    if (m_authBusy) {
      return;
    }

    if (m_options.kscreenlocker || m_options.lock) {
      unlockCurrentSession();
      return;
    }

    const std::string username = m_rootMode ? "root" : selectedProfile().username;
    const std::vector<std::string> command = m_rootMode ? m_rootCommand : selectedSession().command;
    const std::string desktop = m_rootMode ? "root" : selectedSession().desktop;
    std::string password = m_password;
    secureClear(m_password);

    setBusy(true);
    setStatus("");
    try {
      authenticate(username, password, command, desktop);
      secureClear(password);
      m_done = true;
    } catch (const std::exception& error) {
      secureClear(password);
      secureClear(m_password);
      setBusy(false);
      setStatus(error.what()[0] == '\0' ? "Try again" : error.what());
      focusPasswordField();
    }
  }

  void authenticate(
      const std::string& username, const std::string& password, const std::vector<std::string>& command,
      const std::string& desktop
  ) {
    GreetdClient client;
    try {
      auto response = client.request(json{{"type", "create_session"}, {"username", username}});
      bool passwordSent = false;
      while (response.at("type").get<std::string>() == "auth_message") {
        const auto messageType = response.value("auth_message_type", "");
        if (messageType == "secret" || messageType == "visible") {
          response = client.request(json{
              {"type", "post_auth_message_response"},
              {"response", passwordSent ? "" : password},
          });
          passwordSent = true;
        } else {
          response = client.request(json{{"type", "post_auth_message_response"}});
        }
      }
      if (response.at("type").get<std::string>() != "success") {
        throw std::runtime_error(response.value("description", "Authentication failed"));
      }
      const std::vector<std::string> env = {
          "XDG_SESSION_TYPE=wayland",
          "XDG_SESSION_DESKTOP=" + desktop,
          "XDG_CURRENT_DESKTOP=" + desktop,
      };
      response = client.request(json{{"type", "start_session"}, {"cmd", command}, {"env", env}});
      if (response.at("type").get<std::string>() != "success") {
        throw std::runtime_error(response.value("description", "Could not start session"));
      }
    } catch (...) {
      client.cancel();
      throw;
    }
  }

  void unlockCurrentSession() {
    if (!m_authenticationAllowed || m_authBusy || m_password.empty()) {
      return;
    }

    std::string password = m_password;
    secureClear(m_password);
    setBusy(true);
    setStatus("Authenticating");
    try {
      const PamAuthenticator authenticator;
      auto result = authenticator.authenticateCurrentUser(password, m_options.lock ? "noctalia-greetd" : "login");
      secureClear(password);
      if (result.success) {
        setStatus("Unlocked");
        std::cout << "Unlocked" << std::endl;
        m_done = true;
        return;
      }

      setBusy(false);
      setStatus(result.message.empty() ? "Authentication failed" : result.message);
      focusPasswordField();
    } catch (const std::exception& error) {
      secureClear(password);
      setBusy(false);
      setStatus(error.what()[0] == '\0' ? "Authentication failed" : error.what());
      focusPasswordField();
    }
  }

public:
  [[nodiscard]] bool done() const noexcept { return m_done; }

private:
  WaylandConnection& m_wayland;
  LockWidgetServices& m_widgetServices;
  RenderContext& m_renderContext;
  SharedTextureCache& m_textureCache;
  std::unique_ptr<Surface> m_surface;
  std::unique_ptr<OnScreenKeyboard> m_keyboard;
  NativeGreeter* m_authTarget = nullptr;
  wl_output* m_output = nullptr;
  bool m_authenticationAllowed = true;
  std::function<bool()> m_initializeSurface;
  AnimationManager m_animations;
  Node m_root;
  greeter_appearance::LockWidgetLayout m_widgetAppearance;
  std::unique_ptr<LockWidgetsScene> m_lockWidgets;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_tintOverlay = nullptr;
  Box* m_backdrop = nullptr;
  Label* m_clock = nullptr;
  Box* m_loginPanel = nullptr;
  Box* m_passwordFocusRing = nullptr;
  Input* m_passwordField = nullptr;
  Button* m_loginButton = nullptr;
  Label* m_statusLabel = nullptr;
  Button* m_rootButton = nullptr;
  Button* m_profileButton = nullptr;
  Button* m_sessionButton = nullptr;
  Box* m_profileMenu = nullptr;
  Box* m_sessionMenu = nullptr;
  std::vector<Button*> m_profileItems;
  std::vector<Button*> m_sessionItems;
  static constexpr std::size_t kMaxSessionMenuItems = 4;
  std::size_t m_sessionScrollOffset = 0;
  InputDispatcher m_inputDispatcher;
  TextureHandle m_wallpaperTexture{};
  TextureHandle m_blurredWallpaperTexture{};
  BlurCache m_wallpaperBlurCache;
  WallpaperFillMode m_wallpaperFillMode = WallpaperFillMode::Crop;
  Color m_wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  float m_wallpaperBlurIntensity = envFloat("LOCKSCREEN_GREETER_WALLPAPER_BLUR_INTENSITY", 0.0f);
  float m_wallpaperTintIntensity = envFloat("LOCKSCREEN_GREETER_WALLPAPER_TINT_INTENSITY", 0.0f);
  std::vector<Profile> m_profiles;
  std::string m_wallpaperPath;
  std::vector<Session> m_sessions;
  std::vector<std::string> m_rootCommand;
  ProgramOptions m_options;
  KScreenLockerBridge* m_kscreenlockerBridge = nullptr;
  std::string m_password;
  std::string m_statusText;
  std::string m_profileButtonText;
  std::string m_sessionButtonText;
  std::size_t m_selectedProfileIndex = 0;
  std::size_t m_selectedSessionIndex = 0;
  bool m_rootMode = false;
  bool m_authBusy = false;
  bool m_profileMenuOpen = false;
  bool m_sessionMenuOpen = false;
  bool m_wallpaperApplied = false;
  bool m_done = false;
  bool m_clockShadowEnabled = true;
  bool m_loggedPointerEvent = false;
  bool m_loggedKeyboardEvent = false;
};

void runLoop(WaylandConnection& wayland, NativeGreeter& greeter, const std::function<bool()>& tick, const std::function<int()>& pollTimeout, int wakeFd = -1, int appearanceFd = -1, LockWidgetServices* widgetServices = nullptr) {
  while (!greeter.done() && tick()) {
    Surface::drainPendingFrameWork();
    Surface::drainPendingRenders();

    while (wl_display_prepare_read(wayland.display()) != 0) {
      if (wl_display_dispatch_pending(wayland.display()) < 0) {
        throw std::runtime_error(wayland.describeDisplayError(errno));
      }
    }

    int flushRet = 0;
    do {
      flushRet = wl_display_flush(wayland.display());
    } while (flushRet < 0 && errno == EINTR);

    short events = POLLIN;
    if (flushRet < 0) {
      if (errno != EAGAIN) {
        wl_display_cancel_read(wayland.display());
        throw std::runtime_error(wayland.describeDisplayError(errno));
      }
      events |= POLLOUT;
    }

    pollfd fds[4]{{.fd = wl_display_get_fd(wayland.display()), .events = events, .revents = 0},
                  {.fd = wakeFd, .events = POLLIN, .revents = 0},
                  {.fd = appearanceFd, .events = POLLIN, .revents = 0},
                  {.fd = widgetServices ? widgetServices->fd() : -1, .events = POLLIN, .revents = 0}};
    auto& fd = fds[0];
    int timeout = Surface::hasPendingFrameWork() || Surface::hasPendingRenders() ? 0 : pollTimeout();
    if (const int repeatTimeout = wayland.repeatPollTimeoutMs(); repeatTimeout >= 0) {
      timeout = std::min(timeout, repeatTimeout);
    }
    if (const int timerTimeout = TimerManager::instance().pollTimeoutMs(); timerTimeout >= 0) {
      timeout = std::min(timeout, timerTimeout);
    }
    const int pollRet = ::poll(fds, 4, timeout);
    if (pollRet < 0 && errno != EINTR) {
      wl_display_cancel_read(wayland.display());
      throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
    }
    if (pollRet > 0 && (fd.revents & POLLIN) != 0) {
      if (wl_display_read_events(wayland.display()) < 0) {
        throw std::runtime_error(wayland.describeDisplayError(errno));
      }
    } else {
      wl_display_cancel_read(wayland.display());
    }

    if (wl_display_dispatch_pending(wayland.display()) < 0) {
      throw std::runtime_error(wayland.describeDisplayError(errno));
    }
    if (widgetServices && (fds[3].revents & POLLIN)) widgetServices->dispatch();
    TimerManager::instance().tick();
    wayland.repeatTick();
  }
}

} // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  applyPaletteFromFile(envOr("LOCKSCREEN_GREETER_PALETTE"));
  greeter_appearance::Monitor appearance;
  std::string appearanceError;

  const ProgramOptions options = parseOptions(argc, argv);
  auto sessions = (options.keyboard || options.kscreenlocker || options.lock) ? std::vector<Session>{} : parseSessions();
  if (!options.keyboard && !options.kscreenlocker && !options.lock && sessions.empty()) {
    throw std::runtime_error("No sessions configured");
  }
  auto profiles = (options.keyboard || options.kscreenlocker || options.lock) ? currentUserProfile() : parseProfiles();
  if (profiles.empty()) {
    throw std::runtime_error("No user profiles configured");
  }

  KScreenLockerBridge kscreenlockerBridge;
  if (options.kscreenlocker && options.ksldFd >= 0 && !kscreenlockerBridge.initialize(options.ksldFd)) {
    throw std::runtime_error("failed to initialize KScreenLocker bridge");
  }

  WaylandConnection wayland;
  TextInputService textInput;
  wayland.setTextInputService(&textInput);
  wayland.connect();

  GlSharedContext glShared;
  glShared.initialize(wayland.display());
  SharedTextureCache textureCache;
  textureCache.initialize(&glShared);
  RenderContext renderContext;
  renderContext.initialize(glShared);
  renderContext.setTextFontFamily(appearance.value().font);

  // Disable core dumps before any authentication material enters this process.
  const rlimit coreLimit{0, 0};
  setrlimit(RLIMIT_CORE, &coreLimit);
  bool locked = false, finished = false;
  SessionLockHint lockHint;
  ext_session_lock_v1* sessionLock = nullptr;
  std::pair<bool*, bool*> lockState{&locked, &finished};
  if (options.lock) {
    if (!wayland.hasSessionLockManager())
      throw std::runtime_error("session lock protocol unavailable");
    sessionLock = ext_session_lock_manager_v1_lock(wayland.sessionLockManager());
    static const ext_session_lock_v1_listener listener{
      .locked = [](void* data, ext_session_lock_v1*) {
        *static_cast<std::pair<bool*, bool*>*>(data)->first = true;
      },
      .finished = [](void* data, ext_session_lock_v1*) {
        *static_cast<std::pair<bool*, bool*>*>(data)->second = true;
      },
    };
    ext_session_lock_v1_add_listener(sessionLock, &listener, &lockState);
  }

  LockWidgetServices widgetServices;
  if (!options.keyboard) widgetServices.prepare(appearance.value().lockWidgets);
  NativeGreeter greeter(wayland, renderContext, textureCache, profiles,
      envOr("LOCKSCREEN_GREETER_WALLPAPER"), sessions, parseRootCommand(), options,
      options.kscreenlocker ? &kscreenlockerBridge : nullptr, widgetServices);
  greeter.setWidgetAppearance(appearance.value().lockWidgets);
  greeter.setAuthenticationAllowed(!options.lock);
  std::vector<std::unique_ptr<NativeGreeter>> companions;
  widgetServices.changed = [&] {
    greeter.refresh();
    for (auto& view : companions) view->refresh();
  };
  bool outputsChanged = true;
  wayland.setOutputChangeCallback([&] { outputsChanged = true; });
  wayland.setPointerEventCallback([&](const PointerEvent& event) {
    if (event.type == PointerEvent::Type::Motion ||
        (event.type == PointerEvent::Type::Button && pointerEventPressed(event))) {
      greeter.keyboard().clearSelection();
      for (auto& view : companions) view->keyboard().clearSelection();
    }
    greeter.handlePointerEvent(event);
    for (auto& view : companions) view->handlePointerEvent(event);
  });
  wayland.setKeyboardEventCallback([&](const KeyboardEvent& event) { greeter.handleKeyboardEvent(event); });
  ControllerPointer controllers(wayland);
  std::unique_ptr<DesktopKeyboard> desktopKeyboard;
  if (options.keyboard) {
    desktopKeyboard = std::make_unique<DesktopKeyboard>(wayland);
    greeter.onDesktopKey = [&](uint32_t symbol, uint32_t unicode) { desktopKeyboard->send(symbol,unicode); };
  }
  controllers.onNavigate = [&](int dx, int dy) {
    std::vector<NativeGreeter*> views{&greeter};
    for (auto& view : companions) views.push_back(view.get());
    NativeGreeter* target = nullptr;
    for (auto* view : views) if (view->keyboard().hasSelection()) { target = view; break; }
    if (!target) for (auto* view : views)
      if (view->keyboard().visible() && view->ownsSurface(wayland.lastPointerSurface())) { target = view; break; }
    if (!target) for (auto* view : views) if (view->keyboard().visible()) { target = view; break; }
    if (!target) target = companions.empty() ? &greeter : companions.front().get();
    for (auto* view : views) if (view != target) view->keyboard().clearSelection();
    target->keyboard().navigate(dx, dy);
  };
  controllers.onActivate = [&] {
    if (greeter.keyboard().activateSelected()) return true;
    for (auto& view : companions) if (view->keyboard().activateSelected()) return true;
    return false;
  };
  controllers.onCursor = [&] {
    greeter.keyboard().clearSelection();
    for (auto& view : companions) view->keyboard().clearSelection();
  };
  auto lastClock = std::chrono::steady_clock::now();
  auto lastWidgetFrame = lastClock;
  bool readySent = false;
  runLoop(wayland, greeter, [&] {
    if (finished) return false;
    if (appearance.check()) {
      if (!options.keyboard) widgetServices.prepare(appearance.value().lockWidgets);
      renderContext.setTextFontFamily(appearance.value().font);
      greeter.setWidgetAppearance(appearance.value().lockWidgets);
      greeter.refreshAppearance();
      for (auto& view : companions) {
        view->setWidgetAppearance(appearance.value().lockWidgets);
        view->refreshAppearance();
      }
    }
    if (appearance.error() != appearanceError) {
      appearanceError = appearance.error();
      if (!appearanceError.empty()) std::cerr << "noctalia-greetd: appearance update rejected: " << appearanceError << '\n';
    }
    if (!options.keyboard && (!options.lock || locked)) controllers.tick();
    if (desktopKeyboard) desktopKeyboard->commands(
      [&](int x, int y) { greeter.keyboard().navigate(x,y); },
      [&] { greeter.keyboard().activateSelected(); });
    if (outputsChanged) {
      outputsChanged = false;
      const auto outputs = wayland.outputs();
      auto usable = [&](wl_output* output) {
        const auto* info = wayland.findOutputByWl(output);
        return info && info->done && info->hasUsableGeometry();
      };
      std::erase_if(companions, [&](const auto& view) { return !usable(view->output()); });
      if (!usable(greeter.output())) {
        greeter.detach();
        const WaylandOutput* selected = nullptr;
        const auto preferred = envOr("LOCKSCREEN_GREETER_PRIMARY_OUTPUT");
        for (const auto& info : outputs) {
          if (!usable(info.output)) continue;
          if (!selected || info.connectorName == preferred) selected = &info;
          if (info.connectorName == preferred) break;
        }
        if (selected) {
          std::erase_if(companions, [&](const auto& view) { return view->output() == selected->output; });
          if (!greeter.initialize(*selected, sessionLock))
            throw std::runtime_error("failed to initialize primary surface");
        }
      }
      for (const auto& info : outputs) {
        if (options.keyboard || !usable(info.output) || info.output == greeter.output()) continue;
        if (std::any_of(companions.begin(), companions.end(), [&](const auto& view) {
          return view->output() == info.output;
        })) continue;
        auto view = std::make_unique<NativeGreeter>(wayland, renderContext, textureCache, profiles,
          envOr("LOCKSCREEN_GREETER_WALLPAPER"), sessions, parseRootCommand(), options, nullptr, widgetServices);
        view->setWidgetAppearance(appearance.value().lockWidgets);
        view->setAuthTarget(&greeter);
        if (!view->initialize(info, sessionLock))
          throw std::runtime_error("failed to initialize secondary surface");
        companions.push_back(std::move(view));
      }
      greeter.refreshOutputScale();
      for (auto& view : companions) view->refreshOutputScale();
    }
    greeter.setAuthenticationAllowed(!options.lock || locked);
    if (locked && !readySent) {
      lockHint.set(true);
      // systemd Type=notify waits for the compositor's secure-lock acknowledgement.
      // Merely spawning a window is never reported as a completed lock.
      const auto path = envOr("NOTIFY_SOCKET");
      if (!path.empty() && path.size() < sizeof(sockaddr_un::sun_path)) {
        const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size());
        if (address.sun_path[0] == '@') address.sun_path[0] = '\0';
        if (fd >= 0) {
          const char message[] = "READY=1";
          sendto(fd, message, sizeof(message) - 1, MSG_NOSIGNAL,
            reinterpret_cast<sockaddr*>(&address), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + (path.front() == '@' ? 0 : 1)));
          close(fd);
        }
      }
      readySent = true;
    }
    widgetServices.tick();
    const auto now = std::chrono::steady_clock::now();
    const float widgetDelta = std::chrono::duration<float, std::milli>(now-lastWidgetFrame).count();
    if (widgetDelta >= 16.F) {
      lastWidgetFrame = now;
      greeter.widgetFrame(std::min(widgetDelta, 100.F));
      for (auto& view : companions) view->widgetFrame(std::min(widgetDelta, 100.F));
    }
    if (now - lastClock >= std::chrono::seconds(1)) {
      lastClock = now;
      greeter.refresh();
      for (auto& view : companions) view->refresh();
    }
    return true;
  }, [&] {
    bool frame = greeter.needsWidgetFrame();
    for (const auto& view : companions) frame |= view->needsWidgetFrame();
    int timeout = (controllers.active() || frame) ? 16 : 1000;
    const int spectrumTimeout = widgetServices.pollTimeoutMs();
    if (spectrumTimeout >= 0) timeout = std::min(timeout, spectrumTimeout);
    return timeout;
  }, desktopKeyboard ? desktopKeyboard->fd() : -1, appearance.fd(), &widgetServices);
  widgetServices.changed = {};
  controllers.stop();
  if (sessionLock) {
    if (locked && greeter.done() && !finished) {
      ext_session_lock_v1_unlock_and_destroy(sessionLock);
      if (wl_display_roundtrip(wayland.display()) >= 0) lockHint.set(false);
    } else if (!locked) {
      ext_session_lock_v1_destroy(sessionLock);
    }
    // Failure or process termination after locked deliberately leaves the
    // compositor locked. Never turn a failed PAM attempt into an unlock.
  }
  return greeter.done() ? 0 : 1;
}
