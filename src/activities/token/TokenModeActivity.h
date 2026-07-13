#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "network/TokenModeServer.h"

/**
 * TokenModeActivity — scoot-scoot MTG clone-card mode.
 *
 * Joins Wi-Fi (via WifiSelectionActivity), then serves the scoot-scoot PWA
 * and accepts card pushes on POST /display until the user backs out or the
 * idle timeout elapses. See docs/PROTOCOL.md in the scoot-scoot repo.
 *
 * Sleep behavior: preventAutoSleep() holds the device awake only while a
 * push happened within TOKEN_IDLE_TIMEOUT_MS. After that the standard
 * auto-sleep policy (SETTINGS.sleepTimeoutMinutes) takes over, so an idle
 * token eventually deep-sleeps like any other screen.
 */
class TokenModeActivity final : public Activity {
  enum class State {
    WIFI_SELECTION,
    SERVER_RUNNING,
    SHUTTING_DOWN,
  };

  static constexpr unsigned long TOKEN_IDLE_TIMEOUT_MS = 10UL * 60UL * 1000UL;

  State state = State::WIFI_SELECTION;
  std::unique_ptr<TokenModeServer> server;
  std::string connectedIP;
  std::string connectedSSID;
  unsigned long lastHandleClientTime = 0;

  void onWifiSelectionComplete(bool connected);
  void renderIdleScreen() const;

 public:
  explicit TokenModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TokenMode", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return server && server->isRunning(); }
  bool preventAutoSleep() override {
    return server && server->isRunning() && millis() - server->lastRequestMillis() < TOKEN_IDLE_TIMEOUT_MS;
  }
};
