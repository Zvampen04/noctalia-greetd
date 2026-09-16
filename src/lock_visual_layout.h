#pragma once

#include "appearance.h"
#include "config/config_service.h"
#include "render/core/color.h"

#include <cstdint>
#include <optional>
#include <string>

class Box;
class Button;
class Input;
class Label;
class Node;
class Renderer;
class WallpaperNode;

namespace lockscreen {

struct LoginPanelPlacement {
  float cx = 0, cy = 0, width = 520, height = 0;
  bool compact = false;
  std::optional<std::string> backgroundColor;
  float backgroundOpacity = 1.0F;
  std::optional<float> backgroundRadius;
  float inputOpacity = 1.0F;
  std::optional<float> inputRadius;
  bool centerPasswordText = false;
};

// Resolve presentation only. Rotation and flips are deliberately not applied
// to authentication controls because their inverse pointer/focus transform is
// not available in the greeter input dispatcher.
LoginPanelPlacement loginPanelPlacement(
    const greeter_appearance::LockWidget& widget, float outputWidth, float outputHeight);

struct LockVisualLayoutParams {
  Renderer& renderer;
  Node& root;
  Box* backgroundLayer = nullptr;
  WallpaperNode& wallpaper;
  Box& backdrop;
  Box* tintOverlay = nullptr;
  Label& clock;
  Box& loginPanel;
  Input& passwordField;
  Button& loginButton;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float bottomReservation = 0.0f;
  WallpaperFillMode wallpaperFillMode = WallpaperFillMode::Crop;
  Color wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  float tintIntensity = 0.0f;
  bool clockShadowEnabled = true;
  std::optional<LoginPanelPlacement> loginPlacement;
};

struct LockVisualLayoutResult {
  float panelX = 0.0f;
  float panelY = 0.0f;
  float panelWidth = 0.0f;
  float panelHeight = 0.0f;
  float inputX = 0.0F, inputY = 0.0F, inputWidth = 0.0F, controlHeight = 0.0F;
  float buttonX = 0.0F, buttonWidth = 0.0F;
};

LockVisualLayoutResult loginPanelGeometry(float width, float height, float bottomReservation,
    const std::optional<LoginPanelPlacement>& placement);
LockVisualLayoutResult layoutLockVisual(const LockVisualLayoutParams& params);

} // namespace lockscreen
