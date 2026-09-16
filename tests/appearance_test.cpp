#include "appearance.h"
#include "shell/lockscreen/lock_visual_layout.h"

#include <cassert>
#include <limits>
#ifndef NOCTALIA_GREETER_PARSER_ONLY
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "render/scene/wallpaper_node.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#endif
#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
#include "ui/control_settings_palette.h"
#ifndef NOCTALIA_GREETER_PARSER_ONLY
#include "ui/controls/toggle.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/input.h"
#include "ui/controls/box.h"
#include "ui/controls/segmented.h"
#endif
#endif

using greeter_appearance::Json;

namespace {
#ifndef NOCTALIA_GREETER_PARSER_ONLY
class LoginLayoutRenderer final : public Renderer {
public:
  TextMetrics measureText(std::string_view text,float size,FontWeight,float,int,TextAlign,std::string_view,
      TextEllipsize,bool) override {
    return {.width=static_cast<float>(text.size())*size*.5F,.right=static_cast<float>(text.size())*size*.5F,
        .bottom=size,.lineCount=text.empty()?0:1};
  }
  TextMetrics measureFont(float size,FontWeight) override { return {.bottom=size}; }
  void measureTextCursorStops(std::string_view,float,const std::vector<std::size_t>&,std::vector<float>&,FontWeight) override {}
  void measureTextCursorStopsWrapped(std::string_view,float,const std::vector<std::size_t>&,float,
      std::vector<TextCursorStop>&,FontWeight) override {}
  TextMetrics measureGlyph(char32_t,float size) override { return {.width=size,.right=size,.bottom=size}; }
  TextureManager& textureManager() override { std::abort(); }
  float renderScale() const noexcept override { return 1.0F; }
};
#endif
void rejects(const Json& value) {
  bool rejected = false;
  try { (void)greeter_appearance::parse(value); }
  catch (const std::exception&) { rejected = true; }
  assert(rejected);
}

void motionParser() {
  const auto field = [](std::string_view key, Json value) {
    return Json{{"version", 1}, {"animation", {{std::string(key), std::move(value)}}}};
  };
  for (const auto key : {"native", "expressive", "linear", "custom"}) {
    const auto value = greeter_appearance::parse(field("style", key));
    assert(value.motionStyle == *parseMotionStyle(key));
  }
  for (const auto& invalid : {Json("none"), Json("spring"), Json(0), Json(false), Json(nullptr)})
    rejects(field("style", invalid));
  for (const auto key : {"curve_x1", "curve_y1", "curve_x2", "curve_y2"}) {
    const bool isX = std::string_view(key).starts_with("curve_x");
    for (const double value : {0.0, isX ? 1.0 : 2.0, isX ? 0.0 : -2.0}) {
      const auto parsed = greeter_appearance::parse(field(key, value));
      const auto& c = parsed.motionCurve;
      const float actual = std::string_view(key) == "curve_x1" ? c.x1 : std::string_view(key) == "curve_y1" ? c.y1
          : std::string_view(key) == "curve_x2" ? c.x2 : c.y2;
      assert(actual == value);
    }
    for (const auto& invalid : {Json(isX ? -0.01 : -2.01), Json(isX ? 1.01 : 2.01), Json("0"), Json(false),
        Json(nullptr), Json(std::numeric_limits<double>::infinity()), Json(std::numeric_limits<double>::quiet_NaN())})
      rejects(field(key, invalid));
  }
  const auto baseline = greeter_appearance::parse({{"version", 1}});
  assert(baseline.motionStyle == MotionStyle::Native && baseline.motionCurve == MotionCurve{});
  auto disabled = field("style", "custom");
  disabled["animation"]["enabled"] = false;
  assert(!greeter_appearance::parse(disabled).motion);
}

Json lockWidget(std::string id = "clock@DP-1", std::string type = "clock") {
  return {{"id", std::move(id)}, {"type", std::move(type)}, {"output", "DP-1"},
      {"cx", 420.0}, {"cy", 220.0}, {"placement_width", 1920.0}, {"placement_height", 1080.0},
      {"box_width", 280.0}, {"box_height", 120.0}, {"rotation", 0.25}, {"flip_x", true},
      {"flip_y", false}, {"enabled", true},
      {"settings", {{"format", "{:%H:%M}"}, {"center_text", false}, {"background_opacity", 0.0}}}};
}

void lockWidgetParser() {
  auto clock = lockWidget();
  auto login = lockWidget("login@DP-1", "login_box");
  login["settings"] = {{"layout", "compact"}, {"background_opacity", 0.75}, {"center_password_text", true}};
  auto volume = lockWidget("volume@DP-1", "volume");
  volume["settings"] = {{"fill_color", "primary"}, {"show_device", false}};
  auto audio = lockWidget("audio@DP-1", "audio_visualizer");
  audio["settings"] = {{"respect_global_motion", false}, {"show_when_idle", true}};
  auto fancy = lockWidget("fancy@DP-1", "fancy_audio_visualizer");
  fancy["settings"] = {{"respect_global_motion", false}, {"fade_when_idle", false}};
  Json data{{"version", 1}, {"lock_widgets", {{"version", 1}, {"widgets", {clock, login, volume, audio, fancy}}}}};
  const auto parsed = greeter_appearance::parse(data);
  assert(parsed.lockWidgets.widgets.size() == 5);
  const auto& parsedClock = parsed.lockWidgets.widgets[0];
  assert(parsedClock.id == "clock@DP-1" && parsedClock.type == "clock" && parsedClock.output == "DP-1");
  assert(parsedClock.cx == 420.0F && parsedClock.cy == 220.0F && parsedClock.boxWidth == 280.0F);
  assert(parsedClock.rotation == 0.25F && parsedClock.flipX && !parsedClock.flipY && parsedClock.enabled);
  assert(std::get<std::string>(parsedClock.settings.at("format")) == "{:%H:%M}");
  assert(!std::get<bool>(parsedClock.settings.at("center_text")));
  assert(std::get<double>(parsedClock.settings.at("background_opacity")) == 0.0);
  assert(!std::get<bool>(parsed.lockWidgets.widgets[3].settings.at("respect_global_motion")));
  assert(!std::get<bool>(parsed.lockWidgets.widgets[4].settings.at("respect_global_motion")));
  auto endpoint = data;
  endpoint["lock_widgets"]["widgets"][0]["rotation"] = greeter_appearance::kLockWidgetRotationLimit;
  assert(greeter_appearance::parse(endpoint).lockWidgets.widgets[0].rotation
      == static_cast<float>(greeter_appearance::kLockWidgetRotationLimit));
  assert(greeter_appearance::parse({{"version", 1}, {"lock_widgets", {{"version", 1}, {"widgets", Json::array()}}}})
      .lockWidgets.widgets.empty());

  auto invalid = data;
  invalid["lock_widgets"]["version"] = 1.5;
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["enabled"] = true;
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"].push_back(clock);
  rejects(invalid); // Duplicate identity.
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["type"] = "vendor/plugin:lock";
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["type"] = "button";
  invalid["lock_widgets"]["widgets"][0]["settings"] = {{"command", "shutdown now"}};
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["output"] = "../../DP-1";
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["cx"] = std::numeric_limits<double>::infinity();
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["rotation"] = 7.0;
  rejects(invalid);
  for (const auto forbidden : {"command", "device", "interface", "scroll_step", "show_session_buttons",
           "show_login_button"}) {
    invalid = data;
    invalid["lock_widgets"]["widgets"][0]["settings"][forbidden] = "unsafe";
    rejects(invalid);
  }
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["settings"]["format"] = Json::array({"one", "two"});
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"][0]["settings"]["bands"] = 70000;
  invalid["lock_widgets"]["widgets"][0]["type"] = "audio_visualizer";
  rejects(invalid);
  invalid["lock_widgets"]["widgets"][0]["settings"]["bands"] = std::numeric_limits<std::uint64_t>::max();
  rejects(invalid);
  invalid = data;
  invalid["lock_widgets"]["widgets"] = Json::array();
  for (int index = 0; index < 65; ++index)
    invalid["lock_widgets"]["widgets"].push_back(lockWidget("clock@" + std::to_string(index)));
  rejects(invalid);
  rejects({{"version", 1}, {"lock_widgets", {{"version", 1}, {"widgets", Json::array()}}},
      {"session", {{"command", "startplasma-wayland"}}}});
}

void loginPanelPresentation() {
  greeter_appearance::LockWidget widget;
  widget.id = "login@DP-1";
  widget.type = "login_box";
  widget.cx = 960.0F;
  widget.cy = 540.0F;
  widget.placementWidth = 1920.0F;
  widget.placementHeight = 1080.0F;
  widget.boxWidth = 470.0F;
  widget.boxHeight = 180.0F;
  widget.rotation = 1.0F;
  widget.flipX = true;
  widget.settings = {{"layout",std::string("compact")}, {"background_color",std::string("surface_variant")},
      {"background_opacity",0.0}, {"background_radius",std::int64_t{32}}, {"input_opacity",0.4},
      {"input_radius",std::int64_t{8}}, {"center_password_text",true}};
  const auto placement = lockscreen::loginPanelPlacement(widget,1280.0F,720.0F);
  assert(placement.cx == 640.0F && placement.cy == 360.0F);
  assert(placement.width == 470.0F && placement.height == 180.0F && placement.compact);
  assert(placement.backgroundColor == "surface_variant" && placement.backgroundOpacity == 0.0F);
  assert(placement.backgroundRadius == 32.0F && placement.inputOpacity == 0.4F
      && placement.inputRadius == 8.0F && placement.centerPasswordText);
  // Authentication input transforms remain unmodified until inverse pointer
  // mapping exists; the safe resolver consumes geometry/style only.
  assert(widget.rotation == 1.0F && widget.flipX);
  widget.settings["background_opacity"] = 99.0;
  widget.settings["input_radius"] = std::int64_t{-4};
  const auto clamped = lockscreen::loginPanelPlacement(widget,320.0F,200.0F);
  assert(clamped.backgroundOpacity == 1.0F && clamped.inputRadius == 0.0F);
  const auto narrow = lockscreen::loginPanelGeometry(180.0F,140.0F,60.0F,clamped);
  assert(narrow.panelX >= 0.0F && narrow.panelY >= 0.0F && narrow.panelX+narrow.panelWidth <= 180.0F);
  assert(narrow.panelY+narrow.panelHeight <= 80.0F);
  assert(narrow.inputX >= narrow.panelX && narrow.inputY >= narrow.panelY && narrow.inputWidth > 0.0F);
  assert(narrow.buttonX+narrow.buttonWidth <= narrow.panelX+narrow.panelWidth
      && narrow.inputY+narrow.controlHeight <= narrow.panelY+narrow.panelHeight);
}

#ifndef NOCTALIA_GREETER_PARSER_ONLY
void retainedLoginPanelLayout() {
  LoginLayoutRenderer renderer;
  Node root;
  WallpaperNode wallpaper;
  Box backdrop, panel;
  Label clock;
  Input password;
  Button submit;
  password.setPasswordMode(true);
  password.setValue("retained-secret");
  auto placement = lockscreen::LoginPanelPlacement{.cx=160.0F,.cy=100.0F,.width=470.0F,.height=180.0F,
      .compact=true,.backgroundColor=std::string("surface_variant"),.backgroundOpacity=.5F,
      .backgroundRadius=16.0F,.inputOpacity=.4F,.inputRadius=8.0F,.centerPasswordText=true};
  auto narrow = lockscreen::layoutLockVisual({.renderer=renderer,.root=root,.wallpaper=wallpaper,.backdrop=backdrop,
      .clock=clock,.loginPanel=panel,.passwordField=password,.loginButton=submit,.width=320,.height=200,
      .bottomReservation=180.0F,.loginPlacement=placement});
  assert(narrow.panelY+narrow.panelHeight <= 20.0F && password.y()+password.height() <= 20.0F);
  assert(submit.y()+submit.height() <= 20.0F && submit.x()+submit.width() <= narrow.panelX+narrow.panelWidth);
  assert(password.value() == "retained-secret" && panel.style().fill.a == .5F && panel.style().radius.tl == 16.0F);
  auto tiny = lockscreen::layoutLockVisual({.renderer=renderer,.root=root,.wallpaper=wallpaper,.backdrop=backdrop,
      .clock=clock,.loginPanel=panel,.passwordField=password,.loginButton=submit,.width=32,.height=20,
      .loginPlacement=placement});
  assert(tiny.inputWidth > 0.0F && password.width() > 0.0F
      && password.x()+password.width() <= tiny.panelX+tiny.panelWidth);
  assert(submit.x()+submit.width() <= tiny.panelX+tiny.panelWidth && password.value() == "retained-secret");
  auto regular = lockscreen::layoutLockVisual({.renderer=renderer,.root=root,.wallpaper=wallpaper,.backdrop=backdrop,
      .clock=clock,.loginPanel=panel,.passwordField=password,.loginButton=submit,.width=1280,.height=720,
      .loginPlacement=placement});
  assert(regular.panelHeight == 180.0F && password.height() == Style::controlHeight
      && submit.height() == Style::controlHeight && password.value() == "retained-secret");
}
#endif

#ifdef NOCTALIA_GREETER_CARET_APPEARANCE
void caretParser() {
  auto field = [](std::string_view key, Json value) {
    return Json{{"version",1},{"caret",{{std::string(key),std::move(value)}}}};
  };
  for (const auto shape : {"bar","block","underline"})
    assert(greeter_appearance::parse(field("shape",shape)).caret.shape == *parseCaretShape(shape));
  const auto solid = greeter_appearance::parse(field("blink",false));
  assert(!solid.caret.blink);
  for (const auto& invalid : {Json("unknown"),Json(false),Json(0)}) rejects(field("shape",invalid));
  for (const auto key : {"width_px","blink_interval_ms","motion_ms"})
    for (const auto& invalid : {Json(false),Json("1"),Json(-1),Json(nullptr)}) rejects(field(key,invalid));
  assert(greeter_appearance::parse(field("motion_ms",0)).caret.motionMs == 0);
  rejects(field("width_px",9));rejects(field("blink_interval_ms",99));rejects(field("motion_ms",401));
}
#endif

#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
void materialParser() {
  auto field = [](std::string_view key, Json value, std::string_view scope = "") {
    if (scope.empty()) return Json{{"version", 1}, {"material", {{std::string(key), value}}}};
    return Json{{"version", 1}, {"material_overrides", {{std::string(scope),
        {{"lock", {{std::string(key), value}}}}}}}};
  };
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
  for (const float scalar : {low, high, 0.0F}) { \
    if (!noctalia::material::validScalarValue(#key, scalar)) continue; \
    assert(greeter_appearance::parse(field(#key, scalar)).material.key == scalar); \
    for (const auto scope : {"roles", "families", "surfaces"}) { \
      auto parsed = greeter_appearance::parse(field(#key, scalar, scope)); \
      assert(Style::materialScope(parsed.overrides, scope)->at("lock").key == scalar); \
    } \
  } \
  for (const auto& invalid : {Json(static_cast<double>(low) - 1.0), Json(static_cast<double>(high) + 1.0), \
      Json(std::numeric_limits<double>::infinity()), Json(std::numeric_limits<double>::quiet_NaN()), \
      Json("0"), Json(false), Json(nullptr)}) { \
    rejects(field(#key, invalid)); \
    for (const auto scope : {"roles", "families", "surfaces"}) rejects(field(#key, invalid, scope)); \
  }
#include "material/fields.def"
#undef MATERIAL_FIELD
  // Exact discrete plane validation precedes float conversion. These near-zero
  // and near-integer doubles would otherwise round to legal float values.
  for (const double invalid : {-0.5, 0.5, -1.0 + 1e-12, 1.0 - 1e-12, 1e-100}) {
    rejects(field("optical_plane", invalid));
    for (const auto scope : {"roles", "families", "surfaces"}) rejects(field("optical_plane", invalid, scope));
  }
  for (const double plane : {-1.0, 0.0, 1.0})
    assert(greeter_appearance::parse(field("optical_plane", plane)).material.optical_plane == plane);
  for (const double invalid : {-0.5, 0.5, 1.0 - 1e-12, 1e-100}) {
    rejects(field("lens_mapping", invalid));
    for (const auto scope : {"roles", "families", "surfaces"}) rejects(field("lens_mapping", invalid, scope));
  }
  for (const double mapping : {0.0, 1.0})
    assert(greeter_appearance::parse(field("lens_mapping", mapping)).material.lens_mapping == mapping);
  assert(greeter_appearance::parse(field("refraction_radius", 0.0)).material.refraction_radius == 0.0F);
  assert(greeter_appearance::parse(field("refraction_radius", -1.0)).material.refraction_radius == -1.0F);
  assert(greeter_appearance::parse(field("rim_width", 0.0)).material.rim_width == 0.0F);
  assert(greeter_appearance::parse(field("rim_width", -1.0)).material.rim_width == -1.0F);
  assert(greeter_appearance::parse(field("minimum_face_fraction", 0.95)).material.minimum_face_fraction == 0.95F);
  for (const auto scope : {"roles", "families", "surfaces"}) {
    const auto parsed = greeter_appearance::parse(field("minimum_face_fraction", 0.95, scope));
    assert(Style::materialScope(parsed.overrides, scope)->at("lock").minimum_face_fraction == 0.95F);
  }
  assert(greeter_appearance::parse({{"version", 1}}).material == Style::MaterialSettings{});
}
#endif

#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
void controlsParser() {
  const auto field = [](std::string_view key, Json value) {
    return Json{{"version", 1}, {"controls", {{std::string(key), std::move(value)}}}};
  };
  const auto empty = greeter_appearance::parse({{"version", 1}});
  assert(empty.controls == Style::ControlSettings{});
  assert(greeter_appearance::parse({{"version", 1}, {"controls", Json::object()}}).controls == empty.controls);
#define CONTROL_ENUM(member, type, initial, label, group) \
  for (const auto name : Style::controlKeys<Style::type>()) { \
    const auto parsed = greeter_appearance::parse(field(#member, std::string(name))); \
    assert(parsed.controls.member == *Style::parseControl<Style::type>(name)); \
  } \
  for (const auto& invalid : {Json("unknown-choice"), Json("#e0e0e0"), Json(0), Json(false), Json(nullptr)}) \
    rejects(field(#member, invalid));
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) \
  assert(greeter_appearance::parse(field(#member, low)).controls.member == low); \
  assert(greeter_appearance::parse(field(#member, high)).controls.member == high); \
  if (low <= 0.0F && high >= 0.0F) assert(greeter_appearance::parse(field(#member, 0.0)).controls.member == 0.0F); \
  for (const auto& invalid : {Json(static_cast<double>(low) - 1.0), Json(static_cast<double>(high) + 1.0), \
       Json(std::numeric_limits<double>::infinity()), Json(std::numeric_limits<double>::quiet_NaN()), \
       Json("0"), Json(false), Json(nullptr)}) rejects(field(#member, invalid));
#define CONTROL_BOOL(member, initial, label, group) \
  assert(!greeter_appearance::parse(field(#member, false)).controls.member); \
  assert(greeter_appearance::parse(field(#member, true)).controls.member); \
  for (const auto& invalid : {Json(0), Json("false"), Json(nullptr)}) rejects(field(#member, invalid));
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
  for (const auto& invalid : {Json(nullptr), Json::array(), Json("controls")})
    rejects({{"version", 1}, {"controls", invalid}});
  rejects(field("authentication", true));

  // Palette references remain semantic choices through recoloring. Every role
  // permitted by the control schema maps to the corresponding actual palette role.
  for (const auto name : Style::controlKeys<Style::ControlPaletteRole>()) {
    const auto choice = *Style::parseControl<Style::ControlPaletteRole>(name);
    const auto role = std::find_if(kColorRoleTokens.begin(), kColorRoleTokens.end(),
        [name](const auto& candidate) { return candidate.token == name; });
    assert(role != kColorRoleTokens.end() && controlColorRole(choice) == role->role);
  }
  Json colored = field("button_face_role", "primary");
  for (const auto& role : kColorRoleTokens) colored["palette"][std::string(role.token)] = "#234567";
  colored["palette"]["primary"] = "#ff0000";
  const auto red = greeter_appearance::parse(colored);
  colored["palette"]["primary"] = "#0000ff";
  const auto blue = greeter_appearance::parse(colored);
  assert(red.controls == blue.controls && red.colors != blue.colors);
  assert(red.controls.button_face_role == Style::ControlPaletteRole::Primary);
  assert(greeter_appearance::parse({{"version", 1}}).controls == empty.controls);

#ifndef NOCTALIA_GREETER_PARSER_ONLY
  // The normal Meson target also verifies actual retained control instances.
  Toggle toggle;
  Checkbox checkbox;
  Input password;
  Box card;
  Segmented segmented;
  segmented.addOption("First");
  segmented.addOption("Second");
  segmented.setSelectedIndex(1);
  segmented.setEnabled(false);
  auto* segmentedFocus = segmented.focusArea();
  card.setCardStyle();
  password.setPasswordMode(true);
  password.setValue("fixture-password");
  toggle.setCheckedImmediate(true);
  toggle.setEnabled(false);
  checkbox.setChecked(true);
  checkbox.setEnabled(false);
  int changes = 0;
  toggle.setOnChange([&](bool) { ++changes; });
  checkbox.setOnChange([&](bool) { ++changes; });
  segmented.setOnChange([&](std::size_t) { ++changes; });
  const auto changed = greeter_appearance::parse({{"version", 1}, {"surface_material", "neumorphic"},
      {"animation", {{"enabled", false}, {"speed", 0.0}}},
      {"controls", {{"button_variant", "raised_inset"}, {"toggle_variant", "sweep"},
          {"checkbox_variant", "plateau"}, {"card_variant", "raised"},
          {"segmented_variant", "floating"}, {"segmented_transition_ms", 0.0},
          {"segmented_indicator_inset", 0.0}, {"segmented_indicator_opacity", 0.0},
          {"button_transition_ms", 0.0}, {"toggle_transition_ms", 0.0}, {"checkbox_transition_ms", 0.0},
          {"toggle_mirror_rtl", false}, {"checkbox_plateau_tick", false}, {"card_radius", 0.0}}}});
  greeter_appearance::apply(changed);
  assert(Style::controls() == changed.controls);
  assert(!MotionService::instance().enabled());
  assert(toggle.checked() && !toggle.enabled());
  assert(segmented.selectedIndex() == 1 && !segmented.enabled() && segmented.focusArea() == segmentedFocus);
  assert(checkbox.checked() && !checkbox.enabled());
  assert(password.value() == "fixture-password" && changes == 0);
  greeter_appearance::apply(empty);
  assert(Style::controls() == Style::ControlSettings{});
  assert(toggle.checked() && checkbox.checked());
  assert(segmented.selectedIndex() == 1 && !segmented.enabled() && segmented.focusArea() == segmentedFocus);
  assert(password.value() == "fixture-password" && changes == 0);
#endif
}
#endif
}

int main() {
  motionParser();
  lockWidgetParser();
  loginPanelPresentation();
#ifndef NOCTALIA_GREETER_PARSER_ONLY
  retainedLoginPanelLayout();
#endif
  Json data{{"version", 1}, {"font_family", "Nunito Sans"}, {"surface_material", "neumorphic"},
      {"corner_radius_scale", 0.0}, {"button_borders", false}, {"popup_shadows", false},
      {"animation", {{"enabled", false}, {"speed", 0.0}}}};
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  data["design"] = Json::object();
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) data["design"][key] = low;
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
  data["material"] = Json::object();
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) data["material"][#key] = low;
#include "material/fields.def"
#undef MATERIAL_FIELD
  data["material_overrides"] = {{"surfaces", {{"lock", {{"primitive", "liquid_glass"}, {"elevation", 0.0}}}}},
      {"families", {{"button", {{"light_x", 0.0}, {"contact_strength", 0.0}}}}}};
#endif
  const auto parsed = greeter_appearance::parse(data);
  assert(parsed.font == "Nunito Sans" && parsed.primitive == "neumorphic");
  assert(parsed.cornerScale == 0 && !parsed.motion && parsed.motionSpeed == 0);
  assert(!parsed.buttonBorders && !parsed.popupShadows);
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) assert(parsed.metrics.member == low);
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) assert(parsed.material.key == low);
#include "material/fields.def"
#undef MATERIAL_FIELD
  assert(parsed.overrides.surfaces.at("lock").elevation == 0.0F);
  assert(parsed.overrides.families.at("button").light_x == 0.0F);
  const auto defaults = greeter_appearance::parse({{"version", 1}});
  assert(defaults.overrides.surfaces.empty() && defaults.overrides.families.empty());
  assert(defaults.metrics == Style::Metrics{});
  assert(defaults.material == Style::MaterialSettings{});
  auto bad = data;
  bad["material"]["elevation"] = std::numeric_limits<double>::infinity();
  rejects(bad);
  bad = data;
  bad["design"]["anim_fast"] = 1.5;
  rejects(bad);
  bad = data;
  bad["material_overrides"]["surfaces"]["bad target"] = Json::object();
  rejects(bad);
  bad = data;
  bad["material_overrides"]["surfaces"]["lock"]["elevation"] = nullptr;
  rejects(bad);
  bad = data;
  bad["material_overrides"]["families"]["button"]["unknown"] = 1;
  rejects(bad);
#endif
  rejects({{"version", 2}});
  rejects({{"version", 1}, {"authentication", {}}});
  rejects({{"version", 1}, {"surface_material", "Glass"}});
  rejects({{"version", 1}, {"animation", {{"enabled", "false"}}}});
  rejects({{"version", 1}, {"font_family", "bad\nfont"}});
  rejects({{"version", 1}, {"palette", {{"surface", "#123456"}}}});
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
  materialParser();
#endif
#ifdef NOCTALIA_GREETER_CONTROL_APPEARANCE
  controlsParser();
#endif
#ifdef NOCTALIA_GREETER_CARET_APPEARANCE
  caretParser();
#endif
  return 0;
}
