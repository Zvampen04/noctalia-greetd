#pragma once

#include "appearance.h"

#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// MIT licensed, shared by the login and session-lock scenes. Password input
// stays inside this process: no clipboard, subprocess arguments, prediction,
// network service, or persisted keystrokes.
class OnScreenKeyboard {
public:
  using KeyCallback = std::function<void(uint32_t, uint32_t)>;
  OnScreenKeyboard(Node& root, KeyCallback key, std::function<void()> changed)
      : m_key(std::move(key)), m_changed(std::move(changed)) {
    root.addChild(ui::box({.out = &m_panel, .visible = false,
      .configure = [](Box& box) { box.setZIndex(50); }}));
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_panel->setMaterialSurface("osk");
    m_panel->setMaterialIdentity("surface", "panel");
    m_panel->setMaterialBackdrop(MaterialBackdrop::Local);
#endif
    root.addChild(ui::button({.out = &m_toggle, .text = "Keyboard", .glyph = "keyboard",
      .variant = ButtonVariant::Secondary,
      .onClick = [this] { setVisible(!m_visible); },
      .configure = [](Button& button) { button.setZIndex(51); }}));
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_toggle->setMaterialSurface("osk");
#endif
    for (int row = 0; row < 5; ++row) {
      m_rows.emplace_back();
      const int count = row == 4 ? 7 : 12;
      for (int column = 0; column < count; ++column) {
        Button* button = nullptr;
        root.addChild(ui::button({.out = &button, .text = "", .visible = false,
          .onClick = [this, row, column] { activate(row, column); },
          .configure = [](Button& b) { b.setZIndex(51); }}));
        m_rows.back().push_back(button);
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
        button->setMaterialSurface("osk");
#endif
      }
    }
  }
  void setFullDisplay(bool full) { m_fullDisplay = full; }
  void setDesktopMaterial(bool desktop) {
#ifdef NOCTALIA_GREETER_FULL_APPEARANCE
    m_panel->setMaterialBackdrop(desktop ? MaterialBackdrop::Inherited : MaterialBackdrop::Local);
#else
    (void)desktop;
#endif
  }
  void refreshAppearance() {
    m_toggle->setFontSize(Style::fontSizeBody);
    m_toggle->setRadius(Style::scaledRadiusMd());
    for (auto& row : m_rows) for (auto* button : row) {
      button->setRadius(Style::scaledRadiusMd());
      button->markLayoutDirty();
    }
    m_panel->markLayoutDirty();
  }
  bool visible() const { return m_visible; }
  LayoutRect panelRect() const {
    return {m_panel->x(), m_panel->y(), m_panel->width(), m_panel->height()};
  }
  bool hasSelection() const { return m_visible && m_selected; }
  void navigate(int dx, int dy) {
    setVisible(true);
    if (!m_selected) { m_row = 1; m_column = 0; }
    else {
      m_row = (m_row + dy + 5) % 5;
      const int count = static_cast<int>(keysFor(m_row).size());
      m_column = (std::min(m_column, count - 1) + dx + count) % count;
    }
    m_selected = true;
    m_changed();
  }
  bool activateSelected() {
    if (!m_visible || !m_selected) return false;
    activate(m_row, m_column);
    return true;
  }
  void clearSelection() { if (m_selected) { m_selected = false; m_changed(); } }
  void setVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    if (!visible) { m_shift = false; m_symbols = false; m_selected = false; }
    m_changed();
  }
  float reservation(float height) const {
    return m_visible ? (m_fullDisplay ? height - 64.0f : std::min(350.0f, height * 0.46f) + 16.0f) : 0.0f;
  }
  void layout(Renderer& renderer, float width, float height) {
    const float panelH = reservation(height) - 16.0f;
    const float panelW = (m_fullDisplay ? width - 24.0f : std::min(1040.0f, width - 24.0f));
    const float left = (width - panelW) * 0.5f;
    const float top = height - panelH - 12.0f;
    m_panel->setVisible(m_visible);
    m_panel->setPosition(left, top);
    m_panel->setSize(panelW, std::max(0.0f, panelH));
    m_panel->setStyle(RoundedRectStyle{.fill = colorForRole(ColorRole::Surface),
      .border = colorForRole(ColorRole::Outline), .fillMode = FillMode::Solid,
      .radius = Style::scaledRadiusXl(), .borderWidth = Style::cardBordersEnabled() ? Style::borderWidth : 0.0f});
    m_toggle->setText(m_visible ? "Hide keyboard" : "Keyboard");
    m_toggle->setSize(144.0f, Style::controlHeight);
    m_toggle->setPosition(16.0f, 16.0f);
    m_toggle->layout(renderer);
    const float padding = Style::spaceSm;
    const float gap = Style::spaceXs;
    const float keyH = std::max(1.0F, (panelH - padding * 2.0F - gap * 4.0F) / 5.0f);
    for (size_t row = 0; row < m_rows.size(); ++row) {
      const auto keys = keysFor(row);
      const float count = static_cast<float>(keys.size());
      const float keyW = std::max(1.0F, (panelW - padding * 2.0F - gap * (count - 1.0f)) / count);
      for (size_t col = 0; col < m_rows[row].size(); ++col) {
        auto& button = *m_rows[row][col];
        button.setVisible(m_visible && col < keys.size());
        if (col >= keys.size()) continue;
        button.setText(keys[col].label);
        button.setFontSize(std::clamp(std::min(keyH * 0.40f, keyW * 0.60f), Style::fontSizeMini,
            std::max(Style::fontSizeMini, Style::fontSizeTitle)));
        button.setSelected(m_selected && static_cast<int>(row) == m_row && static_cast<int>(col) == m_column);
        button.setSize(keyW, keyH);
        button.setPosition(left + padding + static_cast<float>(col) * (keyW + gap),
            top + padding + static_cast<float>(row) * (keyH + gap));
        button.layout(renderer);
      }
    }
  }
private:
  struct Key { std::string label; uint32_t symbol = 0; uint32_t unicode = 0; };
  std::vector<Key> keysFor(size_t row) const {
    if (row == 4) return {{m_symbols ? "ABC" : "123/#", 1}, {m_shift ? "SHIFT" : "Shift", 2},
      {"Space", XKB_KEY_space, ' '}, {"←", XKB_KEY_Left}, {"→", XKB_KEY_Right},
      {"Enter", XKB_KEY_Return}, {"Hide", 3}};
    const std::u32string letters[] = {U"1234567890", U"qwertyuiopå", U"asdfghjklöä", U"zxcvbnm,.-"};
    const std::u32string symbols[] = {U"!\"#¤%&/()=?", U"@£$€{[]}\\|", U"<>+*_:;~^`", U"'§°±µ©®ßéè"};
    std::vector<Key> result;
    for (uint32_t cp : (m_symbols ? symbols[row] : letters[row])) {
      auto sym = xkb_utf32_to_keysym(cp);
      if (m_shift) { sym = xkb_keysym_to_upper(sym); cp = xkb_keysym_to_utf32(sym); }
      char label[16]{};
      xkb_keysym_to_utf8(sym, label, sizeof(label));
      result.push_back({label, sym, cp});
    }
    if (row == 0) result.push_back({"⌫", XKB_KEY_BackSpace});
    return result;
  }
  void activate(size_t row, size_t col) {
    const auto keys = keysFor(row);
    if (col >= keys.size()) return;
    const auto& key = keys[col];
    if (key.symbol == 1) m_symbols = !m_symbols;
    else if (key.symbol == 2) m_shift = !m_shift;
    else if (key.symbol == 3) setVisible(false);
    else {
      m_key(key.symbol, key.unicode);
      if (key.unicode) m_shift = false;
    }
    m_changed();
  }
  KeyCallback m_key;
  std::function<void()> m_changed;
  Box* m_panel = nullptr;
  Button* m_toggle = nullptr;
  std::vector<std::vector<Button*>> m_rows;
  bool m_fullDisplay = false;
  bool m_selected = false;
  int m_row = 1, m_column = 0;
  bool m_visible = false, m_shift = false, m_symbols = false;
};
