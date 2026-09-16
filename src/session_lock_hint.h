#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

// Inform logind only after the compositor confirms locking/unlocking. Failure
// to publish this advisory state must never prevent the secure surface lock.
class SessionLockHint {
public:
  void set(bool locked) {
    try {
      if (!m_connection) {
        m_connection = sdbus::createSystemBusConnection();
        auto manager = sdbus::createProxy(*m_connection,
            sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});
        sdbus::ObjectPath path;
        const char* session = std::getenv("XDG_SESSION_ID");
        if (session && *session) {
          manager->callMethod("GetSession").onInterface("org.freedesktop.login1.Manager")
              .withArguments(std::string(session)).storeResultsTo(path);
        } else {
          manager->callMethod("GetSessionByPID").onInterface("org.freedesktop.login1.Manager")
              .withArguments(static_cast<std::uint32_t>(getpid())).storeResultsTo(path);
        }
        m_session = sdbus::createProxy(*m_connection, sdbus::ServiceName{"org.freedesktop.login1"}, path);
      }
      if (m_session)
        m_session->callMethod("SetLockedHint").onInterface("org.freedesktop.login1.Session").withArguments(locked);
    } catch (const sdbus::Error& error) {
      std::cerr << "noctalia-greetd: cannot update logind lock state: " << error.what() << '\n';
    }
  }
private:
  std::unique_ptr<sdbus::IConnection> m_connection;
  std::unique_ptr<sdbus::IProxy> m_session;
};
