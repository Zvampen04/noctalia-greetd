#pragma once

#include "ui/style.h"
#include "ui/palette.h"
#include "render/animation/motion_service.h"
#if __has_include(<json.hpp>)
#include <json.hpp>
#else
#include <nlohmann/json.hpp>
#endif
#if __has_include("ui/style_tokens.def") && __has_include("material/fields.def") && __has_include("ui/material_overrides.h")
#define NOCTALIA_GREETER_FULL_APPEARANCE 1
#endif
#if __has_include("ui/control_settings.h") && __has_include("ui/control_settings.def")
#include "ui/control_settings.h"
#define NOCTALIA_GREETER_CONTROL_APPEARANCE 1
#endif
#if __has_include("ui/caret_settings.h")
#include "ui/caret_settings.h"
#include "ui/controls/input.h"
#define NOCTALIA_GREETER_CARET_APPEARANCE 1
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace greeter_appearance {
using Json = nlohmann::json;
inline constexpr double kLockWidgetRotationLimit = static_cast<double>(6.28318530718F);

using LockWidgetSetting = std::variant<bool, std::int64_t, double, std::string>;
struct LockWidget {
  std::string id;
  std::string type;
  std::string output;
  float cx = 0, cy = 0, placementWidth = 0, placementHeight = 0;
  float boxWidth = 0, boxHeight = 0, rotation = 0;
  bool flipX = false, flipY = false, enabled = true;
  std::map<std::string, LockWidgetSetting> settings;
  bool operator==(const LockWidget&) const = default;
};
struct LockWidgetLayout {
  std::vector<LockWidget> widgets;
  bool operator==(const LockWidgetLayout&) const = default;
};

struct Snapshot {
  std::string font = "sans-serif";
  std::string primitive = "flat";
  float cornerScale = 1.0F;
  bool motion = true;
  float motionSpeed = 1.0F;
  MotionStyle motionStyle = MotionStyle::Native;
  MotionCurve motionCurve;
  bool buttonBorders = true, inputBorders = true, cardBorders = true;
  bool popupBorders = true, popupShadows = true;
  std::optional<Palette> colors;
  LockWidgetLayout lockWidgets;
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  Style::Metrics metrics;
  Style::MaterialSettings material;
  Style::MaterialOverrides overrides;
#endif
#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
  Style::ControlSettings controls;
#endif
#ifdef NOCTALIA_GREETER_CARET_APPEARANCE
  CaretSettings caret;
#endif
  bool operator==(const Snapshot&) const = default;
};

inline bool primitiveValid(std::string_view value) {
  return value == "flat" || value == "neumorphic" || value == "liquid_glass" || value == "illustrated";
}
inline double number(const Json& value, double low, double high) {
  if (!value.is_number()) throw std::runtime_error("Appearance value must be numeric");
  const double result = value.get<double>();
  if (!std::isfinite(result) || result < low || result > high)
    throw std::runtime_error("Appearance value is outside its supported range");
  return result;
}
inline bool boolean(const Json& value) {
  if (!value.is_boolean()) throw std::runtime_error("Appearance value must be boolean");
  return value.get<bool>();
}
inline void knownKeys(const Json& value, const std::set<std::string>& keys) {
  if (!value.is_object()) throw std::runtime_error("Appearance section must be an object");
  for (const auto& [key, ignored] : value.items()) {
    (void)ignored;
    if (!keys.contains(key)) throw std::runtime_error("Unknown appearance property: " + key);
  }
}

inline bool safeToken(std::string_view value, std::size_t maximum, std::string_view extra = {}) {
  return !value.empty() && value.size() <= maximum && std::ranges::all_of(value, [extra](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '-' || c == '.' || extra.contains(static_cast<char>(c));
  });
}

inline const std::set<std::string>& lockWidgetSettings(std::string_view type) {
  static const std::set<std::string> kLoginBox{
      "layout", "background_color", "background_opacity", "background_radius", "input_opacity", "input_radius",
      "center_password_text"};
  static const std::set<std::string> kClock{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "clock_style", "format", "center_text", "timezone", "color", "font_family", "shadow", "circle"};
  static const std::set<std::string> kCalendar{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "show_events", "show_week_numbers", "font_family"};
  static const std::set<std::string> kLabel{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "title", "description", "color", "opacity", "font_family", "shadow"};
  static const std::set<std::string> kWeather{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "color", "font_family", "shadow", "show_forecast", "forecast_days"};
  static const std::set<std::string> kMedia{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "layout", "color", "font_family", "shadow", "hide_when_no_media"};
  static const std::set<std::string> kSysmon{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "stat", "stat2", "network_speed_unit", "network_speed_compact", "display", "gauge_layout", "color", "color2",
      "highlight_color", "font_family", "show_label", "label_min_width", "shadow"};
  static const std::set<std::string> kVolume{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "glyph", "fill_color", "track_color", "show_device", "font_family", "shadow"};
  static const std::set<std::string> kStatus{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "color", "show_label", "font_family"};
  static const std::set<std::string> kAudio{
      "background", "background_color", "background_opacity", "background_radius", "background_padding", "bands",
      "mirrored", "reversed", "centered", "show_when_idle", "gap_ratio", "corner_radius", "reflection_height",
      "reflection_opacity", "smoothing_ms", "respect_global_motion", "color_1", "color_2"};
  static const std::set<std::string> kFancyAudio{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "visualization_mode", "sensitivity", "rotation_speed", "bar_width", "wave_thickness", "ring_opacity",
      "inner_diameter", "bloom_intensity", "fade_when_idle", "respect_global_motion", "primary_color", "secondary_color"};
  if (type == "login_box") return kLoginBox;
  if (type == "clock") return kClock;
  if (type == "calendar") return kCalendar;
  if (type == "label") return kLabel;
  if (type == "weather") return kWeather;
  if (type == "media_player") return kMedia;
  if (type == "sysmon") return kSysmon;
  if (type == "volume") return kVolume;
  if (type == "battery" || type == "brightness") return kStatus;
  if (type == "audio_visualizer") return kAudio;
  return kFancyAudio;
}

inline bool lockWidgetTypeAllowed(std::string_view type) {
  constexpr std::array allowed{
      "login_box", "clock", "calendar", "label", "weather", "media_player", "sysmon", "volume", "battery",
      "brightness", "audio_visualizer", "fancy_audio_visualizer"};
  return std::ranges::find(allowed, type) != allowed.end();
}

inline LockWidgetSetting lockWidgetSetting(const Json& value) {
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_unsigned()) {
    const auto result = value.get<std::uint64_t>();
    if (result > 65536) throw std::runtime_error("Lock widget setting is outside its supported range");
    return static_cast<std::int64_t>(result);
  }
  if (value.is_number_integer()) {
    const auto result = value.get<std::int64_t>();
    if (result < -65536 || result > 65536) throw std::runtime_error("Lock widget setting is outside its supported range");
    return result;
  }
  if (value.is_number_float()) return number(value, -65536, 65536);
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    if (text.size() > 256 || std::ranges::any_of(text, [](unsigned char c) { return c < 32 && c != '\n'; }))
      throw std::runtime_error("Invalid lock widget setting string");
    return text;
  }
  throw std::runtime_error("Lock widget settings must be scalar presentation values");
}

inline LockWidgetLayout parseLockWidgets(const Json& value) {
  knownKeys(value, {"version", "widgets"});
  if (!value.contains("version") || !value["version"].is_number_integer() || value["version"] != 1
      || !value.contains("widgets") || !value["widgets"].is_array() || value["widgets"].size() > 64)
    throw std::runtime_error("Invalid lock widget layout");
  LockWidgetLayout result;
  std::set<std::string> ids;
  for (const auto& item : value["widgets"]) {
    knownKeys(item, {"id", "type", "output", "cx", "cy", "placement_width", "placement_height", "box_width",
        "box_height", "rotation", "flip_x", "flip_y", "enabled", "settings"});
    for (const auto key : {"id", "type", "output", "cx", "cy", "placement_width", "placement_height", "box_width",
             "box_height", "rotation", "flip_x", "flip_y", "enabled", "settings"})
      if (!item.contains(key)) throw std::runtime_error("Incomplete lock widget definition");
    LockWidget widget;
    if (!item["id"].is_string() || !item["type"].is_string() || !item["output"].is_string())
      throw std::runtime_error("Invalid lock widget identity");
    widget.id = item["id"].get<std::string>();
    widget.type = item["type"].get<std::string>();
    widget.output = item["output"].get<std::string>();
    if (!safeToken(widget.id, 64, "@") || !ids.insert(widget.id).second || !lockWidgetTypeAllowed(widget.type)
        || (!widget.output.empty() && !safeToken(widget.output, 128, ":@")))
      throw std::runtime_error("Invalid or duplicate lock widget identity");
    widget.cx = number(item["cx"], -65536, 65536);
    widget.cy = number(item["cy"], -65536, 65536);
    widget.placementWidth = number(item["placement_width"], 0, 16384);
    widget.placementHeight = number(item["placement_height"], 0, 16384);
    widget.boxWidth = number(item["box_width"], 0, 16384);
    widget.boxHeight = number(item["box_height"], 0, 16384);
    widget.rotation = number(item["rotation"], -kLockWidgetRotationLimit, kLockWidgetRotationLimit);
    widget.flipX = boolean(item["flip_x"]);
    widget.flipY = boolean(item["flip_y"]);
    widget.enabled = boolean(item["enabled"]);
    const auto& settings = item["settings"];
    if (!settings.is_object() || settings.size() > 32) throw std::runtime_error("Invalid lock widget settings");
    const auto& allowed = lockWidgetSettings(widget.type);
    for (const auto& [key, setting] : settings.items()) {
      if (!allowed.contains(key)) throw std::runtime_error("Unsupported lock widget setting: " + key);
      widget.settings.emplace(key, lockWidgetSetting(setting));
    }
    result.widgets.push_back(std::move(widget));
  }
  return result;
}

#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
inline float materialNumber(const Json& value, std::string_view key, double /*low*/, double /*high*/) {
  if (!value.is_number()) throw std::runtime_error("Appearance material value must be numeric");
  const double scalar = value.get<double>();
  // Shared validation checks exact discrete values before conversion, while
  // continuous bounds use the actual float representation stored by the core.
  if (!noctalia::material::validScalarValue(key, scalar))
    throw std::runtime_error("Appearance material value is outside its supported range or discrete choices");
  return static_cast<float>(scalar);
}
inline Style::MaterialOverride patch(const Json& value) {
  std::set<std::string> keys{"primitive"};
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) keys.insert(#key);
#include "material/fields.def"
#undef MATERIAL_FIELD
  knownKeys(value, keys);
  Style::MaterialOverride result;
  if (value.contains("primitive")) {
    const auto name = value.at("primitive").get<std::string>();
    result.primitive = Style::materialPrimitive(name);
    if (!result.primitive) throw std::runtime_error("Unknown appearance material primitive");
  }
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
  if (value.contains(#key)) result.key = materialNumber(value.at(#key), #key, low, high);
#include "material/fields.def"
#undef MATERIAL_FIELD
  return result;
}
inline Style::MaterialOverrideMap scope(const Json& value) {
  if (!value.is_object() || value.size() > 128) throw std::runtime_error("Invalid appearance override scope");
  Style::MaterialOverrideMap result;
  for (const auto& [target, fields] : value.items()) {
    if (!Style::validMaterialTarget(target)) throw std::runtime_error("Invalid appearance override target");
    result.emplace(target, patch(fields));
  }
  return result;
}
#endif

inline Snapshot parse(const Json& value) {
  knownKeys(value, {"version", "font_family", "surface_material", "corner_radius_scale", "animation",
      "button_borders", "input_borders", "card_borders", "popup_borders", "popup_shadows",
      "design", "material", "material_overrides", "controls", "caret", "palette", "lock_widgets"});
  if (!value.contains("version") || !value["version"].is_number_integer() || value["version"] != 1)
    throw std::runtime_error("Unsupported appearance snapshot version");
  Snapshot result;
  if (value.contains("lock_widgets")) result.lockWidgets = parseLockWidgets(value["lock_widgets"]);
#ifdef NOCTALIA_GREETER_CARET_APPEARANCE
  if (value.contains("caret")) {
    const auto& caret = value["caret"];
    knownKeys(caret, {"shape", "width_px", "blink", "blink_interval_ms", "motion_ms"});
    if (caret.contains("shape")) {
      if (!caret["shape"].is_string()) throw std::runtime_error("Appearance caret shape must be a string");
      const auto shape = parseCaretShape(caret["shape"].get<std::string>());
      if (!shape) throw std::runtime_error("Unknown appearance caret shape");
      result.caret.shape = *shape;
    }
    if (caret.contains("width_px")) result.caret.widthPx = number(caret["width_px"], 1, 8);
    if (caret.contains("blink")) result.caret.blink = boolean(caret["blink"]);
    if (caret.contains("blink_interval_ms")) result.caret.blinkIntervalMs = number(caret["blink_interval_ms"], 100, 2000);
    if (caret.contains("motion_ms")) result.caret.motionMs = number(caret["motion_ms"], 0, 400);
  }
#else
  if (value.contains("caret")) throw std::runtime_error("This greeter core cannot apply caret appearance");
#endif
  if (value.contains("font_family")) {
    result.font = value["font_family"].get<std::string>();
    if (result.font.empty()) result.font = "sans-serif";
    if (result.font.size() > 256 || std::any_of(result.font.begin(), result.font.end(),
        [](unsigned char c) { return c < 32 || c == 127; })) throw std::runtime_error("Invalid appearance font family");
  }
  if (value.contains("surface_material")) result.primitive = value["surface_material"].get<std::string>();
  if (!primitiveValid(result.primitive)) throw std::runtime_error("Unknown appearance material primitive");
  if (value.contains("corner_radius_scale")) result.cornerScale = number(value["corner_radius_scale"], 0, 2);
  if (value.contains("animation")) {
    const auto& animation = value["animation"];
    knownKeys(animation, {"enabled", "speed", "style", "curve_x1", "curve_y1", "curve_x2", "curve_y2"});
    if (animation.contains("enabled")) result.motion = boolean(animation["enabled"]);
    if (animation.contains("speed")) result.motionSpeed = number(animation["speed"], 0, 4);
    if (animation.contains("style")) {
      if (!animation["style"].is_string()) throw std::runtime_error("Appearance motion style must be a string");
      const auto style = parseMotionStyle(animation["style"].get<std::string>());
      if (!style) throw std::runtime_error("Unknown appearance motion style");
      result.motionStyle = *style;
    }
    if (animation.contains("curve_x1")) result.motionCurve.x1 = number(animation["curve_x1"], 0, 1);
    if (animation.contains("curve_y1")) result.motionCurve.y1 = number(animation["curve_y1"], -2, 2);
    if (animation.contains("curve_x2")) result.motionCurve.x2 = number(animation["curve_x2"], 0, 1);
    if (animation.contains("curve_y2")) result.motionCurve.y2 = number(animation["curve_y2"], -2, 2);
  }
  const std::array booleans{
      std::pair{"button_borders", &Snapshot::buttonBorders}, std::pair{"input_borders", &Snapshot::inputBorders},
      std::pair{"card_borders", &Snapshot::cardBorders}, std::pair{"popup_borders", &Snapshot::popupBorders},
      std::pair{"popup_shadows", &Snapshot::popupShadows}};
  for (const auto& [key, member] : booleans) if (value.contains(key)) result.*member = boolean(value[key]);
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  if (value.contains("design")) {
    const auto& fields = value["design"];
    std::set<std::string> keys;
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) keys.insert(key);
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
    knownKeys(fields, keys);
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) \
    if (fields.contains(key)) { const double scalar = number(fields[key], low, high); \
      if constexpr (std::is_integral_v<type>) { if (std::floor(scalar) != scalar) throw std::runtime_error("Appearance metric must be integral"); } \
      result.metrics.member = static_cast<type>(scalar); }
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
  }
  if (value.contains("material")) {
    const auto parsed = patch(value["material"]);
    if (parsed.primitive) throw std::runtime_error("Global primitive belongs in surface_material");
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) if (parsed.key) result.material.key = *parsed.key;
#include "material/fields.def"
#undef MATERIAL_FIELD
  }
  if (value.contains("material_overrides")) {
    const auto& overrides = value["material_overrides"];
    knownKeys(overrides, {"roles", "families", "surfaces"});
    if (overrides.contains("roles")) result.overrides.roles = scope(overrides["roles"]);
    if (overrides.contains("families")) result.overrides.families = scope(overrides["families"]);
    if (overrides.contains("surfaces")) result.overrides.surfaces = scope(overrides["surfaces"]);
  }
#endif
  if (value.contains("controls")) {
#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
    const auto& controls = value["controls"];
    std::set<std::string> keys;
#define CONTROL_ENUM(member, type, initial, label, group) keys.insert(#member);
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) keys.insert(#member);
#define CONTROL_BOOL(member, initial, label, group) keys.insert(#member);
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
    knownKeys(controls, keys);
#define CONTROL_ENUM(member, type, initial, label, group) \
    if (controls.contains(#member)) { \
      if (!controls[#member].is_string()) throw std::runtime_error("Appearance control choice must be a string"); \
      const auto parsed = Style::parseControl<Style::type>(controls[#member].get<std::string>()); \
      if (!parsed) throw std::runtime_error("Unknown appearance control choice: " #member); \
      result.controls.member = *parsed; }
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) \
    if (controls.contains(#member)) result.controls.member = static_cast<float>(number(controls[#member], low, high));
#define CONTROL_BOOL(member, initial, label, group) \
    if (controls.contains(#member)) result.controls.member = boolean(controls[#member]);
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
#else
    throw std::runtime_error("This greeter does not support configurable control appearance");
#endif
  }
  if (value.contains("palette")) {
    const std::array colors{
      std::pair{"primary", &Palette::primary}, std::pair{"on_primary", &Palette::onPrimary},
      std::pair{"secondary", &Palette::secondary}, std::pair{"on_secondary", &Palette::onSecondary},
      std::pair{"tertiary", &Palette::tertiary}, std::pair{"on_tertiary", &Palette::onTertiary},
      std::pair{"error", &Palette::error}, std::pair{"on_error", &Palette::onError},
      std::pair{"surface", &Palette::surface}, std::pair{"on_surface", &Palette::onSurface},
      std::pair{"surface_variant", &Palette::surfaceVariant}, std::pair{"on_surface_variant", &Palette::onSurfaceVariant},
      std::pair{"outline", &Palette::outline}, std::pair{"shadow", &Palette::shadow},
      std::pair{"hover", &Palette::hover}, std::pair{"on_hover", &Palette::onHover}};
    std::set<std::string> keys;
    for (const auto& [key, member] : colors) { (void)member; keys.insert(key); }
    const auto& fields = value["palette"];
    knownKeys(fields, keys);
    if (fields.size() != colors.size()) throw std::runtime_error("Appearance palette must contain every color role");
    Palette next{};
    for (const auto& [key, member] : colors) {
      const auto text = fields.at(key).get<std::string>();
      if (text.size() != 7 || text.front() != '#' || !std::all_of(text.begin() + 1, text.end(),
          [](unsigned char c) { return std::isxdigit(c); })) throw std::runtime_error("Invalid appearance palette color");
      const auto rgb = std::stoul(text.substr(1), nullptr, 16);
      next.*member = rgba(((rgb >> 16) & 255) / 255.0F, ((rgb >> 8) & 255) / 255.0F, (rgb & 255) / 255.0F, 1.0F);
    }
    result.colors = next;
  }
  return result;
}

inline Snapshot legacyEnvironment() {
  Snapshot result;
  const auto env = [](const char* name) { const auto* text = std::getenv(name); return text ? std::string(text) : std::string{}; };
  if (const auto font = env("LOCKSCREEN_GREETER_FONT_FAMILY"); !font.empty()) result.font = font;
  if (const auto material = env("LOCKSCREEN_GREETER_SURFACE_MATERIAL"); primitiveValid(material)) result.primitive = material;
  result.motion = env("LOCKSCREEN_GREETER_ANIMATIONS") != "false";
  try {
    const auto text = env("LOCKSCREEN_GREETER_CORNER_SCALE");
    if (!text.empty()) { const auto value = std::stof(text); if (std::isfinite(value)) result.cornerScale = std::clamp(value, 0.0F, 2.0F); }
  } catch (const std::exception&) {}
  result.buttonBorders = result.inputBorders = result.cardBorders = result.primitive != "neumorphic" && result.primitive != "liquid_glass";
  return result;
}

inline void apply(const Snapshot& value) {
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  Style::setMetrics(value.metrics);
  Style::setMaterialSettings(value.material);
  Style::setMaterialOverrides(value.overrides);
  Style::setSurfaceMaterial(value.primitive == "neumorphic" ? Style::SurfaceMaterialMode::Neumorphic
      : value.primitive == "liquid_glass" ? Style::SurfaceMaterialMode::LiquidGlass
      : value.primitive == "illustrated" ? Style::SurfaceMaterialMode::Illustrated : Style::SurfaceMaterialMode::Flat);
#elif defined(NOCTALIA_HAS_SURFACE_MATERIALS)
  Style::setSurfaceMaterial(value.primitive == "neumorphic" ? Style::SurfaceMaterialMode::Neumorphic
      : value.primitive == "liquid_glass" ? Style::SurfaceMaterialMode::LiquidGlass : Style::SurfaceMaterialMode::Flat);
#endif
#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
  Style::setControls(value.controls);
#endif
#ifdef NOCTALIA_GREETER_CARET_APPEARANCE
  Input::setCaretSettings(value.caret);
#endif
  Style::setCornerRadiusScale(value.cornerScale);
  Style::setButtonBordersEnabled(value.buttonBorders);
  Style::setInputBordersEnabled(value.inputBorders);
  Style::setCardBordersEnabled(value.cardBorders);
  Style::setPopupBordersEnabled(value.popupBorders);
  Style::setPopupShadowsEnabled(value.popupShadows);
  bool enabled = value.motion && value.motionSpeed > 0;
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  enabled = enabled && (value.metrics.animFast > 0 || value.metrics.animNormal > 0 || value.metrics.animSlow > 0);
#endif
  MotionService::instance().setEnabled(enabled); // Finishes/cancels registered active animations when disabled.
  MotionService::instance().setSpeed(value.motionSpeed > 0 ? value.motionSpeed : 1.0F);
  MotionService::instance().setStyle(value.motionStyle, value.motionCurve);
  if (value.colors) setPalette(*value.colors);
}

inline std::filesystem::path selectedPath() {
  if (const auto* path = std::getenv("LOCKSCREEN_GREETER_APPEARANCE_FILE"); path && *path) return path;
  if (const auto* state = std::getenv("NOCTALIA_GREETER_STATE_DIR"); state && *state) return std::filesystem::path(state) / "appearance.json";
  return "/var/lib/noctalia-greeter/appearance.json";
}

class Monitor {
public:
  explicit Monitor(std::filesystem::path path = selectedPath()) : m_path(std::move(path)), m_value(legacyEnvironment()) {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    watch();
    reload();
    apply(m_value);
  }
  ~Monitor() { if (m_fd >= 0) close(m_fd); }
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;
  int fd() const { return m_fd; }
  const Snapshot& value() const { return m_value; }
  const std::string& error() const { return m_error; }
  bool check() {
    bool changed = false;
    alignas(inotify_event) char events[4096];
    ssize_t count;
    while (m_fd >= 0 && (count = read(m_fd, events, sizeof(events))) > 0) {
      for (ssize_t offset = 0; offset < count;) {
        const auto* event = reinterpret_cast<const inotify_event*>(events + offset);
        if (event->wd == m_watch || (event->mask & IN_Q_OVERFLOW) != 0) {
          if ((event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
            if ((event->mask & IN_IGNORED) == 0 && m_watch >= 0) inotify_rm_watch(m_fd, m_watch);
            m_watch = -1;
          }
          if (event->len == 0 || m_path.filename() == event->name) changed = true;
        }
        offset += sizeof(inotify_event) + event->len;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_watch < 0 && now >= m_nextRetry) {
      m_nextRetry = now + std::chrono::seconds(1);
      watch();
      changed = true;
    }
    if (!changed || !reload()) return false;
    apply(m_value);
    return true;
  }
private:
  void watch() {
    if (m_fd >= 0 && m_watch < 0) m_watch = inotify_add_watch(m_fd, m_path.parent_path().c_str(),
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
  }
  bool reload() {
    const int file = open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) return false; // Missing replacement retains the last valid appearance.
    struct Close { int fd; ~Close() { close(fd); } } closeFile{file};
    try {
      struct stat info{};
      if (fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 2 * 1024 * 1024)
        throw std::runtime_error("Appearance snapshot must be a regular file below 2 MiB");
      std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = read(file, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) throw std::runtime_error("Cannot read appearance snapshot");
        offset += static_cast<std::size_t>(count);
      }
      auto next = parse(Json::parse(bytes));
      m_error.clear();
      if (next == m_value) return false;
      m_value = std::move(next);
      return true;
    } catch (const std::exception& error) { m_error = error.what(); return false; }
  }
  std::filesystem::path m_path;
  Snapshot m_value;
  std::string m_error;
  int m_fd = -1, m_watch = -1;
  std::chrono::steady_clock::time_point m_nextRetry{};
};
} // namespace greeter_appearance
