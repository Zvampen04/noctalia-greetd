#include "shell/lockscreen/lock_visual_layout.h"

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

LockVisualLayoutResult layoutLockVisual(const LockVisualLayoutParams& params) {
  const float sw = static_cast<float>(params.width);
  const float sh = static_cast<float>(params.height);
  const float panelWidth = std::min(sw - Style::spaceLg * 2.0f, 520.0f);
  const float panelHeight = 78.0f;
  const float panelX = std::round((sw - panelWidth) * 0.5f);
  const float panelY = std::max(Style::spaceLg, sh - panelHeight - 84.0f);

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
  params.backdrop.setSize(sw, sh);
  params.backdrop.setVisible(params.wallpaperFillColor.a > 0.0f);
  params.backdrop.setStyle(RoundedRectStyle{
      .fill = params.wallpaperFillColor,
      .fillMode = FillMode::Solid,
  });

  if (params.tintOverlay != nullptr) {
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

  constexpr float kClockFontSize = 64.0f;
  params.clock.setFontSize(kClockFontSize);
  params.clock.setFontWeight(FontWeight::Bold);
  params.clock.measure(params.renderer);
  const float clockX = sw - 48.0f - params.clock.width();
  const float clockY = 86.0f;

  params.clockShadow.setVisible(params.clockShadowEnabled);
  params.clockShadow.setFontSize(kClockFontSize);
  params.clockShadow.setFontWeight(FontWeight::Bold);
  params.clockShadow.setColor(colorSpecFromRole(ColorRole::Shadow, 0.55f));
  params.clockShadow.setText(params.clock.text());
  params.clockShadow.measure(params.renderer);
  params.clockShadow.setPosition(clockX + 3.0f, clockY + 4.0f);
  params.clock.setPosition(clockX, clockY);

  params.loginPanel.setPosition(panelX, panelY);
  params.loginPanel.setSize(panelWidth, panelHeight);
  params.loginPanel.setStyle(RoundedRectStyle{
      .fill = colorForRole(ColorRole::SurfaceVariant, 0.88f),
      .border = colorForRole(ColorRole::Outline, 0.95f),
      .fillMode = FillMode::Solid,
      .radius = Style::scaledRadiusXl(),
      .softness = 1.0f,
      .borderWidth = Style::borderWidth,
  });

  const float contentLeft = panelX + Style::spaceLg;
  const float contentTop = panelY + 22.0f;
  const float rightInset = Style::spaceLg + Style::spaceSm;
  const float contentWidth = panelWidth - Style::spaceLg - rightInset;
  const float buttonWidth = Style::controlHeight;
  const float gap = Style::spaceSm;
  const float inputWidth = std::max(120.0f, contentWidth - buttonWidth - gap);

  params.passwordField.setSize(inputWidth, 0.0f);
  params.passwordField.setPosition(contentLeft, contentTop);
  params.passwordField.layout(params.renderer);

  params.loginButton.setSize(buttonWidth, Style::controlHeight);
  params.loginButton.setPosition(contentLeft + inputWidth + gap, contentTop);
  params.loginButton.layout(params.renderer);

  return LockVisualLayoutResult{
      .panelX = panelX,
      .panelY = panelY,
      .panelWidth = panelWidth,
      .panelHeight = panelHeight,
  };
}

} // namespace lockscreen
