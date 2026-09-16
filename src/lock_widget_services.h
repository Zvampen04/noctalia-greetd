#pragma once
#include "appearance.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "shell/desktop/desktop_widget_services.h"
#include "system/system_monitor_service.h"
#include <iostream>
#include <memory>

// One bundle per process, shared by every output. Owners must destroy the scenes
// before this bundle. Services use this process's runtime, never a selected user's.
class LockWidgetServices {
public:
  void prepare(const greeter_appearance::LockWidgetLayout& layout) {
    bool needsMonitor = false, needsAudio = false;
    for (const auto& spec : layout.widgets) {
      if (!spec.enabled) continue;
      needsMonitor |= spec.type == "sysmon";
      needsAudio |= spec.type == "volume" || spec.type == "audio_visualizer"
          || spec.type == "fancy_audio_visualizer";
    }
    if (needsMonitor && !m_monitor && !m_monitorAttempted) {
      m_monitorAttempted = true;
      try { m_monitor = std::make_unique<SystemMonitorService>(); }
      catch (const std::exception&) {
        std::cerr << "noctalia-greetd: system monitor unavailable\n";
      }
    }
    if (m_monitor) m_monitor->setEnabled(needsMonitor);
    if (needsAudio && !m_audio && !m_audioAttempted) {
      m_audioAttempted = true;
      try {
        m_audio = std::make_unique<PipeWireService>();
        m_spectrum = std::make_unique<PipeWireSpectrum>(*m_audio);
        m_audio->setChangeCallback([this] {
          m_spectrum->handleAudioStateChanged();
          if (changed) changed();
        });
      } catch (const std::exception&) {
        m_spectrum.reset(); m_audio.reset();
        std::cerr << "noctalia-greetd: audio widgets unavailable\n";
      }
    }
  }
  DesktopWidgetRuntimeServices runtime() const {
    return {.pipewire = m_audio.get(), .pipewireSpectrum = m_spectrum.get(), .sysmon = m_monitor.get()};
  }
  int fd() const { return m_audio ? m_audio->fd() : -1; }
  void dispatch() { if (m_audio) m_audio->dispatch(); }
  void tick() { if (m_spectrum) m_spectrum->tick(); }
  int pollTimeoutMs() const { return m_spectrum ? m_spectrum->pollTimeoutMs() : -1; }
  std::function<void()> changed;
private:
  std::unique_ptr<SystemMonitorService> m_monitor;
  std::unique_ptr<PipeWireService> m_audio;
  std::unique_ptr<PipeWireSpectrum> m_spectrum;
  bool m_monitorAttempted = false;
  bool m_audioAttempted = false;
};
