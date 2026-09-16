#include "shell/lockscreen/lock_visual_layout.h"

#include "config/color_spec.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/wallpaper_node.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace lockscreen {

namespace {
  template <typename T>
  std::optional<T> setting(const greeter_appearance::LockWidget& widget, std::string_view key) {
    const auto it = widget.settings.find(std::string(key));
    if (it == widget.settings.end()) return std::nullopt;
    if (const auto* value = std::get_if<T>(&it->second)) return *value;
    return std::nullopt;
  }

  std::optional<float> numericSetting(const greeter_appearance::LockWidget& widget, std::string_view key) {
    if (const auto value = setting<double>(widget, key)) return static_cast<float>(*value);
    if (const auto value = setting<std::int64_t>(widget, key)) return static_cast<float>(*value);
    return std::nullopt;
  }
}

LoginPanelPlacement loginPanelPlacement(
    const greeter_appearance::LockWidget& widget, float outputWidth, float outputHeight) {
  const float sx = widget.placementWidth > 0 ? outputWidth / std::max(1.0F,widget.placementWidth) : 1.0F;
  const float sy = widget.placementHeight > 0 ? outputHeight / std::max(1.0F,widget.placementHeight) : 1.0F;
  LoginPanelPlacement result{
      .cx = widget.cx * sx,
      .cy = widget.cy * sy,
      .width = widget.boxWidth,
      .height = widget.boxHeight,
  };
  if (const auto value = setting<std::string>(widget, "layout")) result.compact = *value == "compact";
  result.backgroundColor = setting<std::string>(widget, "background_color");
  if (const auto value = numericSetting(widget, "background_opacity"))
    result.backgroundOpacity = std::clamp(*value, 0.0F, 1.0F);
  if (const auto value = numericSetting(widget, "background_radius"))
    result.backgroundRadius = std::clamp(*value, 0.0F, 32.0F);
  if (const auto value = numericSetting(widget, "input_opacity"))
    result.inputOpacity = std::clamp(*value, 0.0F, 1.0F);
  if (const auto value = numericSetting(widget, "input_radius"))
    result.inputRadius = std::clamp(*value, 0.0F, 32.0F);
  if (const auto value = setting<bool>(widget, "center_password_text")) result.centerPasswordText = *value;
  return result;
}

LockVisualLayoutResult loginPanelGeometry(float width, float height, float bottomReservation,
    const std::optional<LoginPanelPlacement>& placement) {
  const bool compact = placement && placement->compact;
  const float defaultWidth = compact ? 400.0F : 520.0F;
  const float minimumWidth = compact ? 240.0F : 320.0F;
  const float requestedWidth = placement && placement->width > 0 ? placement->width : defaultWidth;
  const float panelWidth = std::min(std::max(1.0F,width - Style::spaceLg * 2.0f),
      std::max(minimumWidth,requestedWidth));
  const float minimumHeight = Style::controlHeight + 2.0F * Style::spaceLg;
  const float availableHeight = std::max(1.0F, height - bottomReservation);
  const float requestedHeight = placement && placement->height > 0 ? placement->height : minimumHeight;
  const float panelHeight = std::min(availableHeight,std::max(minimumHeight,requestedHeight));
  const float maxX = std::max(0.0F,width-panelWidth);
  const float maxY = std::max(0.0F,height-bottomReservation-panelHeight);
  const float panelX = placement ? std::clamp(placement->cx-panelWidth*.5F,0.0F,maxX)
      : std::round((width-panelWidth)*.5F);
  const float panelY = placement ? std::clamp(placement->cy-panelHeight*.5F,0.0F,maxY)
      : std::min(maxY,std::max(Style::spaceLg,height-bottomReservation-panelHeight-84.0F));
  const float controlHeight = std::min(Style::controlHeight,panelHeight);
  const float horizontalPadding = std::min(Style::spaceLg,std::max(0.0F,(panelWidth-controlHeight)*.25F));
  const float contentLeft = panelX + horizontalPadding;
  const float contentTop = panelY + std::max(0.0F,(panelHeight-controlHeight)*.5F);
  const float contentWidth = std::max(1.0F,panelWidth-horizontalPadding*2.0F);
  const float buttonWidth = std::min(controlHeight,std::max(0.0F,contentWidth*.25F));
  const float reservedInputWidth = contentWidth * .25F;
  const float gap = std::min(Style::spaceSm,
      std::max(0.0F,contentWidth-buttonWidth-reservedInputWidth));
  const float inputWidth = std::max(0.0F,contentWidth-buttonWidth-gap);
  return {.panelX=panelX,.panelY=panelY,.panelWidth=panelWidth,.panelHeight=panelHeight,
      .inputX=contentLeft,.inputY=contentTop,.inputWidth=inputWidth,.controlHeight=controlHeight,
      .buttonX=contentLeft+inputWidth+gap,.buttonWidth=buttonWidth};
}

LockVisualLayoutResult layoutLockVisual(const LockVisualLayoutParams& params) {
  const float sw = static_cast<float>(params.width);
  const float sh = static_cast<float>(params.height);
  const auto geometry = loginPanelGeometry(sw, sh, params.bottomReservation, params.loginPlacement);
  const float panelX = geometry.panelX, panelY = geometry.panelY;
  const float panelWidth = geometry.panelWidth, panelHeight = geometry.panelHeight;

  params.root.setSize(sw, sh);

  if (params.backgroundLayer != nullptr) {
    params.backgroundLayer->setPosition(0.0f, 0.0f);
    params.backgroundLayer->setSize(sw, sh);
  }

  params.wallpaper.setPosition(0.0f, 0.0f);
  params.wallpaper.setSize(sw, sh);
  params.wallpaper.setFillMode(params.wallpaperFillMode);
  params.wallpaper.setFillColor(params.wallpaperFillColor);

  params.backdrop.setPosition(0.0f, 0.0f);
#ifdef NOCTALIA_HAS_SURFACE_MATERIALS
  params.backdrop.setSurfaceRelief(0.0F);
#endif
  params.backdrop.setSize(sw, sh);
  params.backdrop.setVisible(params.wallpaperFillColor.a > 0.0f);
  params.backdrop.setStyle(RoundedRectStyle{
      .fill = params.wallpaperFillColor,
      .fillMode = FillMode::Solid,
  });

  if (params.tintOverlay != nullptr) {
#ifdef NOCTALIA_HAS_SURFACE_MATERIALS
    params.tintOverlay->setSurfaceRelief(0.0F);
#endif
    params.tintOverlay->setPosition(0.0f, 0.0f);
    params.tintOverlay->setSize(sw, sh);
    const bool showTint = params.tintIntensity > 0.0f;
    params.tintOverlay->setVisible(showTint);
    if (showTint) {
      params.tintOverlay->setStyle(RoundedRectStyle{
          .fill = colorForRole(ColorRole::Surface, params.tintIntensity),
          .fillMode = FillMode::Solid,
      });
    }
  }

  const bool compactClock = sw < 600.0f || params.bottomReservation > sh * 0.7f;
  const float kClockFontSize = Style::fontSizeHeader * (compactClock ? 1.67F : 2.67F);
  params.clock.setFontSize(kClockFontSize);
  params.clock.setFontWeight(FontWeight::Bold);
  params.clock.measure(params.renderer);
  const float clockX = sw - (sw < 600.0f ? 16.0f : 48.0f) - params.clock.width();
  const float clockY = compactClock ? 12.0f : std::min(86.0f, std::max(16.0f, panelY - 80.0f));

  // Keep shadow and glyphs on the same text node and measured layout.
  if (params.clockShadowEnabled && Style::popupShadowsEnabled())
    params.clock.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.55f), 3.0f, 4.0f);
  else
    params.clock.clearShadow();
  params.clock.setPosition(clockX, clockY);

  params.loginPanel.setPosition(panelX, panelY);
  params.loginPanel.setSize(panelWidth, panelHeight);
  Color panelFill = colorForRole(ColorRole::Surface);
  if (params.loginPlacement && params.loginPlacement->backgroundColor) {
    try {
      panelFill = resolveColorSpec(
          colorSpecFromConfigString(*params.loginPlacement->backgroundColor, "login_box.background_color"));
    }
    catch (const std::exception&) { panelFill = colorForRole(ColorRole::Surface); }
  }
  panelFill.a *= params.loginPlacement ? params.loginPlacement->backgroundOpacity : 1.0F;
  params.loginPanel.setStyle(RoundedRectStyle{
      .fill = panelFill,
      .border = colorForRole(ColorRole::Outline, 0.95f),
      .fillMode = FillMode::Solid,
      .radius = params.loginPlacement && params.loginPlacement->backgroundRadius
          ? *params.loginPlacement->backgroundRadius : Style::scaledRadiusXl(),
      .softness = 1.0f,
      .borderWidth = Style::cardBordersEnabled() ? Style::borderWidth : 0.0F,
  });

  params.passwordField.setSurfaceOpacity(params.loginPlacement ? params.loginPlacement->inputOpacity : 1.0F);
  params.passwordField.setFrameRadius(params.loginPlacement && params.loginPlacement->inputRadius
      ? *params.loginPlacement->inputRadius : Style::radiusMd);
  params.passwordField.setTextAlign(params.loginPlacement && params.loginPlacement->centerPasswordText
      ? TextAlign::Center : TextAlign::Start);
  params.passwordField.setControlHeight(geometry.controlHeight);
  params.passwordField.setSize(geometry.inputWidth, geometry.controlHeight);
  params.passwordField.setPosition(geometry.inputX, geometry.inputY);
  params.passwordField.layout(params.renderer);

  params.loginButton.setControlHeight(geometry.controlHeight);
  params.loginButton.setMinWidth(geometry.buttonWidth);
  params.loginButton.setMaxWidth(geometry.buttonWidth);
  params.loginButton.setSize(geometry.buttonWidth, geometry.controlHeight);
  params.loginButton.setPosition(geometry.buttonX, geometry.inputY);
  params.loginButton.layout(params.renderer);

  return geometry;
}

} // namespace lockscreen
