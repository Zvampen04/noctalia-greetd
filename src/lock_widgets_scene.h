#pragma once
#include "appearance.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "render/core/renderer.h"
#include <algorithm>
#include <vector>
#include <map>
#include <memory>
#include <set>

// Appearance-only, non-interactive widgets. Authentication remains in NativeGreeter.
class LockWidgetsScene {
public:
  struct Callbacks {
    std::function<void()> update, layout, redraw, frame;
  };
  explicit LockWidgetsScene(Node& root, DesktopWidgetRuntimeServices services = {},
      Callbacks callbacks = {}, AnimationManager* animations = nullptr)
      : m_root(root), m_factory(services, DesktopWidgetFactory::MissingServicePolicy::RenderUnavailable),
        m_services(services), m_callbacks(std::move(callbacks)), m_animations(animations) {}
  void setServices(DesktopWidgetRuntimeServices services) {
    if (services.pipewire == m_services.pipewire && services.pipewireSpectrum == m_services.pipewireSpectrum
        && services.sysmon == m_services.sysmon) return;
    clear();
    m_services = services;
    m_factory = DesktopWidgetFactory(services, DesktopWidgetFactory::MissingServicePolicy::RenderUnavailable);
  }
  bool needsFrameTick() const {
    for (const auto& [id, entry] : m_entries) if (entry.widget->needsFrameTick()) return true;
    return false;
  }
  void frameTick(float deltaMs, Renderer& renderer) {
    for (auto& [id, entry] : m_entries)
      if (entry.widget->needsFrameTick()) entry.widget->onFrameTick(deltaMs, renderer);
  }
  ~LockWidgetsScene() { clear(); }
  void sync(const greeter_appearance::LockWidgetLayout& layout, std::string_view output,
            bool primary, Renderer& renderer, float width, float height) {
    std::set<std::string> retained;
    std::vector<Node*> ordered;
    m_hasClock = false;
    for (const auto& spec : layout.widgets) {
      if (!spec.enabled || (spec.output.empty() ? !primary : spec.output != output)) continue;
      // Do not expose the general factory's scripts, personal data or login controls.
      if (spec.type != "clock" && spec.type != "label" && spec.type != "calendar"
          && spec.type != "sysmon" && spec.type != "volume" && spec.type != "audio_visualizer"
          && spec.type != "fancy_audio_visualizer") continue;
      retained.insert(spec.id);
      auto it = m_entries.find(spec.id);
      if (it != m_entries.end() && it->second.spec != spec) {
        remove(it->second);
        m_entries.erase(it);
        it = m_entries.end();
      }
      if (it == m_entries.end()) {
        Entry entry;
        entry.spec = spec;
        std::unordered_map<std::string, WidgetSettingValue> settings;
        for (const auto& [key,value] : spec.settings)
          std::visit([&](const auto& v) { settings.emplace(key, WidgetSettingValue{v}); }, value);
        // Never load personal calendar data into the login scene. Factory construction
        // also applies constructor-only options, such as sysmon's secondary series.
        if (spec.type == "calendar") settings["show_events"] = false;
        entry.widget = m_factory.create(spec.type, settings);
        if (!entry.widget) continue;
        entry.widget->setAnimationManager(m_animations);
        entry.widget->setUpdateCallback(m_callbacks.update);
        entry.widget->setLayoutCallback(m_callbacks.layout);
        entry.widget->setRedrawCallback(m_callbacks.redraw);
        entry.widget->setFrameTickRequestCallback(m_callbacks.frame);
        entry.widget->create();
        for (const auto& [key,value] : settings) {
          if (key == "background" || key.starts_with("background_")) continue;
          entry.widget->applySetting(key,value,settings,renderer);
        }
        auto wrapper = std::make_unique<Node>();
        wrapper->setHitTestVisible(false);
        wrapper->setMaterialBackdropLocal(true);
        wrapper->setZIndex(1);
        entry.presentation = wrapper->addChild(entry.widget->releaseRoot());
        entry.wrapper = m_root.addChild(std::move(wrapper));
        it = m_entries.emplace(spec.id, std::move(entry)).first;
      }
      auto& entry = it->second;
      ordered.push_back(entry.wrapper);
      entry.widget->setBox(spec.boxWidth, spec.boxHeight);
      entry.widget->update(renderer);
      entry.widget->layout(renderer);
      // The released outer node owns box size/background padding. root() is
      // the inner content, which can be smaller and positioned within it.
      const float w = entry.presentation->width(), h = entry.presentation->height();
      const float cx = spec.cx * (spec.placementWidth > 0 ? width / std::max(1.F,spec.placementWidth) : 1);
      const float cy = spec.cy * (spec.placementHeight > 0 ? height / std::max(1.F,spec.placementHeight) : 1);
      entry.wrapper->setFrameSize(w,h);
      entry.wrapper->setPosition(cx-w*.5F,cy-h*.5F);
      entry.wrapper->setTransformOrigin(w*.5F,h*.5F);
      entry.wrapper->setRotation(spec.rotation);
      entry.wrapper->setScale(spec.flipX ? -1.F : 1.F,spec.flipY ? -1.F : 1.F);
      m_hasClock |= spec.type == "clock";
    }
    for (auto it=m_entries.begin();it!=m_entries.end();) {
      if (!retained.contains(it->first)) { remove(it->second);it=m_entries.erase(it); }
      else ++it;
    }
    // JSON/widget order is presentation stacking order. Preserve retained nodes
    // and authentication siblings while updating equal-z decoration order.
    std::vector<Node*> current;
    for (const auto& child : m_root.children())
      if (std::ranges::find(ordered, child.get()) != ordered.end()) current.push_back(child.get());
    if (current != ordered) {
      for (auto* wrapper : ordered) {
        auto node = m_root.removeChild(wrapper);
        m_root.addChild(std::move(node));
      }
    }
  }
  bool hasClock() const { return m_hasClock; }
private:
  struct Entry {
    greeter_appearance::LockWidget spec;
    std::unique_ptr<DesktopWidget> widget;
    Node* wrapper = nullptr;
    Node* presentation = nullptr;
  };
  void remove(Entry& entry) {
    // Widget callbacks are destroyed while their released scene nodes still exist.
    entry.widget.reset();
    m_root.removeChild(entry.wrapper);
  }
  void clear() { for (auto& [id,entry] : m_entries) remove(entry);m_entries.clear(); }
  Node& m_root;
  DesktopWidgetFactory m_factory;
  DesktopWidgetRuntimeServices m_services;
  Callbacks m_callbacks;
  AnimationManager* m_animations = nullptr;
  std::map<std::string,Entry> m_entries;
  bool m_hasClock = false;
};
