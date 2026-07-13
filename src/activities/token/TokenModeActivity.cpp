#include "TokenModeActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* TAG = "TOKENACT";
constexpr const char* MDNS_HOSTNAME = "crosspoint";
constexpr int QR_SIZE = 198;
}  // namespace

void TokenModeActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG(TAG, "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  state = State::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& wifi = std::get<WifiResult>(result.data);
                             connectedIP = wifi.ip;
                             connectedSSID = wifi.ssid;
                           }
                           onWifiSelectionComplete(!result.isCancelled);
                         });
}

void TokenModeActivity::onExit() {
  Activity::onExit();
  state = State::SHUTTING_DOWN;
  server.reset();
  MDNS.end();

  // Same teardown as CrossPointWebServerActivity: a restart is the only
  // reliable way to fully release the Wi-Fi stack on this platform.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void TokenModeActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    onGoHome();
    return;
  }

  MDNS.end();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    LOG_DBG(TAG, "mDNS started: http://%s.local/", MDNS_HOSTNAME);
  } else {
    LOG_DBG(TAG, "WARNING: mDNS failed to start");
  }

  // Card BMPs are portrait; render everything in portrait so pushed images
  // and the idle screen share one coordinate system.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  server = makeUniqueNoThrow<TokenModeServer>(renderer);
  if (!server) {
    LOG_ERR(TAG, "OOM: TokenModeServer");
    onGoHome();
    return;
  }
  server->begin();
  if (!server->isRunning()) {
    LOG_ERR(TAG, "Failed to start token mode server");
    server.reset();
    onGoHome();
    return;
  }

  state = State::SERVER_RUNNING;
  requestUpdate();
}

void TokenModeActivity::loop() {
  if (state != State::SERVER_RUNNING || !server || !server->isRunning()) return;

  // Pump HTTP in a tight loop for throughput, with watchdog resets and
  // responsive back-button handling (same pattern as CrossPointWebServerActivity).
  esp_task_wdt_reset();
  constexpr int MAX_ITERATIONS = 500;
  for (int i = 0; i < MAX_ITERATIONS && server->isRunning(); i++) {
    server->handleClient();
    if ((i & 0x1F) == 0x1F) esp_task_wdt_reset();
    if ((i & 0x3F) == 0x3F) {
      yield();
      mappedInput.update();
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onGoHome();
        return;
      }
    }
  }
  lastHandleClientTime = millis();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // Idle: leave token mode (exits via onGoHome -> onExit tears down Wi-Fi);
  // the standard auto-sleep policy then puts the device to deep sleep.
  if (millis() - server->lastRequestMillis() >= TOKEN_IDLE_TIMEOUT_MS) {
    LOG_DBG(TAG, "Token mode idle for %lu ms; leaving token mode", TOKEN_IDLE_TIMEOUT_MS);
    onGoHome();
  }
}

void TokenModeActivity::render(RenderLock&&) {
  if (state != State::SERVER_RUNNING) return;
  // Never repaint over a pushed card — the card IS the UI once it arrives.
  if (server && server->hasDisplayedImage()) return;
  renderIdleScreen();
}

void TokenModeActivity::renderIdleScreen() const {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TOKEN_MODE), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 3;
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TOKEN_MODE_HINT), true, EpdFontFamily::BOLD);
  y += height10 + metrics.verticalSpacing * 2;

  const std::string url = std::string("http://") + MDNS_HOSTNAME + ".local/";
  QrUtils::drawQrCode(renderer, Rect{(pageWidth - QR_SIZE) / 2, y, QR_SIZE, QR_SIZE}, url);
  y += QR_SIZE + metrics.verticalSpacing * 2;

  renderer.drawCenteredText(UI_10_FONT_ID, y, url.c_str(), true);
  y += height10 + 5;
  const std::string ipUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
  renderer.drawCenteredText(SMALL_FONT_ID, y, ipUrl.c_str(), true);

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
