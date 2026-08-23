#pragma once

#include "config/config_service.h"
#include "render/core/color.h"

#include <cstdint>

class Box;
class Button;
class Input;
class Label;
class Node;
class Renderer;
class WallpaperNode;

namespace lockscreen {

struct LockVisualLayoutParams {
  Renderer& renderer;
  Node& root;
  Box* backgroundLayer = nullptr;
  WallpaperNode& wallpaper;
  Box& backdrop;
  Box* tintOverlay = nullptr;
  Label& clockShadow;
  Label& clock;
  Box& loginPanel;
  Input& passwordField;
  Button& loginButton;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  WallpaperFillMode wallpaperFillMode = WallpaperFillMode::Crop;
  Color wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  float tintIntensity = 0.0f;
  bool clockShadowEnabled = true;
};

struct LockVisualLayoutResult {
  float panelX = 0.0f;
  float panelY = 0.0f;
  float panelWidth = 0.0f;
  float panelHeight = 0.0f;
};

LockVisualLayoutResult layoutLockVisual(const LockVisualLayoutParams& params);

} // namespace lockscreen
