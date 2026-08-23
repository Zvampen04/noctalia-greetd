#include "auth/pam_authenticator.h"
#if __has_include("core/input/key_symbols.h")
#include "core/input/key_symbols.h"
#else
#include "core/key_symbols.h"
#endif
#include "core/timer_manager.h"
#include "render/core/color.h"
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
    if (arg == "--kscreenlocker") {
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
      std::vector<std::string> rootCommand, ProgramOptions options, KScreenLockerBridge* kscreenlockerBridge
  )
      : m_wayland(wayland),
        m_renderContext(renderContext),
        m_textureCache(textureCache),
        m_profiles(std::move(profiles)),
        m_wallpaperPath(std::move(wallpaper)),
        m_sessions(std::move(sessions)),
        m_rootCommand(std::move(rootCommand)),
        m_options(options),
        m_kscreenlockerBridge(kscreenlockerBridge) {
    applyPaletteFromFile(envOr("LOCKSCREEN_GREETER_PALETTE"));
    Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t) { return KeySymbol::isEnter(sym); });
    buildScene();
  }

  bool initialize() {
    wl_output* output = nullptr;
    std::uint32_t defaultWidth = 1920;
    std::uint32_t defaultHeight = 1080;
    if (!m_wayland.outputs().empty()) {
      const auto& firstOutput = m_wayland.outputs().front();
      output = firstOutput.output;
      if (firstOutput.logicalWidth > 0) {
        defaultWidth = static_cast<std::uint32_t>(firstOutput.logicalWidth);
      }
      if (firstOutput.logicalHeight > 0) {
        defaultHeight = static_cast<std::uint32_t>(firstOutput.logicalHeight);
      }
    }

    if (m_wayland.hasLayerShell() && !envBool("LOCKSCREEN_GREETER_FORCE_TOPLEVEL", false)) {
      LayerSurfaceConfig config{
          .nameSpace = "noctalia-greetd",
          .layer = LayerShellLayer::Overlay,
          .anchor =
              LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = 0,
          .exclusiveZone = -1,
          .keyboard = LayerShellKeyboard::Exclusive,
          .defaultWidth = defaultWidth,
          .defaultHeight = defaultHeight,
      };
      auto layerSurface = std::make_unique<LayerSurface>(m_wayland, std::move(config));
      m_surface = std::move(layerSurface);
      m_initializeSurface = [this, output]() { return static_cast<LayerSurface*>(m_surface.get())->initialize(output); };
    } else {
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

    m_surface->setRenderContext(&m_renderContext);
    m_surface->setSceneRoot(&m_root);
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
    m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });

    if (!m_initializeSurface || !m_initializeSurface()) {
      return false;
    }
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
    if (m_surface != nullptr) {
      wl_surface* focusedSurface = m_wayland.lastKeyboardSurface();
      if (focusedSurface != nullptr && focusedSurface != m_surface->wlSurface()) {
        return;
      }
    }

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

    m_root.addChild(ui::label({.out = &m_clockShadow}));
    m_root.addChild(ui::label({
        .out = &m_clock,
        .color = colorSpecFromRole(ColorRole::Primary),
    }));

    m_root.addChild(ui::box({.out = &m_loginPanel}));
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
    applyWallpaperTexture();

    const float sw = static_cast<float>(width);
    const float sh = static_cast<float>(height);
    const auto visual = lockscreen::layoutLockVisual(lockscreen::LockVisualLayoutParams{
        .renderer = layoutRenderer(*m_surface, m_renderContext),
        .root = m_root,
        .wallpaper = *m_wallpaper,
        .backdrop = *m_backdrop,
        .tintOverlay = m_tintOverlay,
        .clockShadow = *m_clockShadow,
        .clock = *m_clock,
        .loginPanel = *m_loginPanel,
        .passwordField = *m_passwordField,
        .loginButton = *m_loginButton,
        .width = width,
        .height = height,
        .wallpaperFillMode = m_wallpaperFillMode,
        .wallpaperFillColor = m_wallpaperFillColor,
        .tintIntensity = m_wallpaperTintIntensity,
        .clockShadowEnabled = m_clockShadowEnabled,
    });

    layoutPasswordFocusRing();
    layoutStatusLabel(visual);

    if (m_options.kscreenlocker) {
      hideGreeterControls();
      return;
    }

    const float extraButtonH = 42.0f;
    const float extraBottom = 32.0f;
    const float edgeInset = 32.0f;
    const float buttonGap = 8.0f;
    const float sessionButtonW = 220.0f;
    const float profileButtonW = 220.0f;
    const float buttonY = sh - extraBottom - extraButtonH;
    const float profileX = sw - edgeInset - profileButtonW;
    const float sessionX = profileX - buttonGap - sessionButtonW;

    m_rootButton->setText(m_rootMode ? "User login" : "Root login");
    m_rootButton->setSize(128.0f, extraButtonH);
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
        .fill = colorForRole(ColorRole::SurfaceVariant, 0.96f),
        .border = colorForRole(ColorRole::Outline, 0.95f),
        .fillMode = FillMode::Solid,
        .radius = Style::scaledRadiusXl(),
        .softness = 1.0f,
        .borderWidth = Style::borderWidth,
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
        .fill = colorForRole(ColorRole::SurfaceVariant, 0.96f),
        .border = colorForRole(ColorRole::Outline, 0.95f),
        .fillMode = FillMode::Solid,
        .radius = Style::scaledRadiusXl(),
        .softness = 1.0f,
        .borderWidth = Style::borderWidth,
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
    if (parseColorWallpaperPath(m_wallpaperPath, color)) {
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

    if (m_options.kscreenlocker) {
      unlockCurrentSession();
      return;
    }

    const std::string username = m_rootMode ? "root" : selectedProfile().username;
    const std::vector<std::string> command = m_rootMode ? m_rootCommand : selectedSession().command;
    const std::string desktop = m_rootMode ? "root" : selectedSession().desktop;
    const std::string password = m_password;

    setBusy(true);
    setStatus("");
    try {
      authenticate(username, password, command, desktop);
      m_done = true;
    } catch (const std::exception& error) {
      m_password.clear();
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
    if (m_authBusy || m_password.empty()) {
      return;
    }

    std::string password = m_password;
    secureClear(m_password);
    setBusy(true);
    setStatus("Authenticating");
    try {
      const PamAuthenticator authenticator;
      auto result = authenticator.authenticateCurrentUser(password, "login");
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
  RenderContext& m_renderContext;
  SharedTextureCache& m_textureCache;
  std::unique_ptr<Surface> m_surface;
  std::function<bool()> m_initializeSurface;
  Node m_root;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_tintOverlay = nullptr;
  Box* m_backdrop = nullptr;
  Label* m_clockShadow = nullptr;
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
  Color m_wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
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

void runLoop(WaylandConnection& wayland, NativeGreeter& greeter) {
  while (greeter.running() && !greeter.done()) {
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

    pollfd fd{.fd = wl_display_get_fd(wayland.display()), .events = events, .revents = 0};
    int timeout = Surface::hasPendingFrameWork() || Surface::hasPendingRenders() ? 0 : 1000;
    if (const int repeatTimeout = wayland.repeatPollTimeoutMs(); repeatTimeout >= 0) {
      timeout = std::min(timeout, repeatTimeout);
    }
    if (const int timerTimeout = TimerManager::instance().pollTimeoutMs(); timerTimeout >= 0) {
      timeout = std::min(timeout, timerTimeout);
    }
    const int pollRet = ::poll(&fd, 1, timeout);
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
    TimerManager::instance().tick();
    wayland.repeatTick();
  }
}

} // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  const ProgramOptions options = parseOptions(argc, argv);
  auto sessions = options.kscreenlocker ? std::vector<Session>{} : parseSessions();
  if (!options.kscreenlocker && sessions.empty()) {
    throw std::runtime_error("No sessions configured");
  }
  auto profiles = options.kscreenlocker ? currentUserProfile() : parseProfiles();
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

  NativeGreeter greeter(
      wayland, renderContext, textureCache, std::move(profiles), envOr("LOCKSCREEN_GREETER_WALLPAPER"),
      std::move(sessions), parseRootCommand(), options, options.kscreenlocker ? &kscreenlockerBridge : nullptr
  );
  wayland.setPointerEventCallback([&greeter](const PointerEvent& event) { greeter.handlePointerEvent(event); });
  wayland.setKeyboardEventCallback([&greeter](const KeyboardEvent& event) { greeter.handleKeyboardEvent(event); });

  if (!greeter.initialize()) {
    throw std::runtime_error("failed to initialize native greeter surface");
  }

  std::cerr << "noctalia-greetd: native greeter initialized\n";
  if (options.kscreenlocker) {
    std::cout << "Locked at " << std::time(nullptr) << std::endl;
  }
  runLoop(wayland, greeter);
  std::cerr << "noctalia-greetd: native greeter exiting\n";
  return 0;
}
