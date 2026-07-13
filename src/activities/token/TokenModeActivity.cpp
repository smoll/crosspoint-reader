#include "TokenModeActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* TAG = "TOKENACT";
constexpr const char* MDNS_HOSTNAME = "crosspoint";
constexpr const char* AP_SSID = "ScootScoot";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_SIZE = 172;

// Stable per-device WPA2 password (>= 8 chars), derived from the factory MAC
// so it survives reflashes and can live on a sticker or the join QR.
std::string deriveApPassword() {
  const uint64_t mac = ESP.getEfuseMac();
  char pass[16];
  snprintf(pass, sizeof(pass), "scoot-%04x", static_cast<unsigned>((mac >> 24) & 0xFFFF));
  return pass;
}
}  // namespace

void TokenModeActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG(TAG, "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  state = State::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();

  // Zero-touch entry: try saved networks silently; no saved networks means
  // the picker can't help, so go straight to the hotspot.
  if (WIFI_STORE.getCredentials().empty()) {
    startHotspot();
  } else {
    startWifiSelection(true);
  }
}

void TokenModeActivity::onExit() {
  Activity::onExit();
  state = State::SHUTTING_DOWN;
  stopServer();
  MDNS.end();

  // Same teardown as CrossPointWebServerActivity: a restart is the only
  // reliable way to fully release the Wi-Fi stack on this platform.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (apMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }
}

void TokenModeActivity::startWifiSelection(const bool autoConnectOnly) {
  state = State::WIFI_SELECTION;
  stopServer();
  if (apMode) {
    WiFi.softAPdisconnect(true);
    apMode = false;
  }
  WiFi.mode(WIFI_STA);

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, autoConnectOnly),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             // Nothing connectable (or the user backed out): host our own network.
                             startHotspot();
                             return;
                           }
                           const auto& wifi = std::get<WifiResult>(result.data);
                           connectedIP = wifi.ip;
                           connectedSSID = wifi.ssid;
                           apMode = false;
                           startServer();
                         });
}

void TokenModeActivity::startHotspot() {
  stopServer();
  LOG_DBG(TAG, "Starting token hotspot...");
  WiFi.mode(WIFI_AP);
  delay(100);

  apPassword = deriveApPassword();
  if (!WiFi.softAP(AP_SSID, apPassword.c_str(), AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    LOG_ERR(TAG, "Failed to start hotspot");
    onGoHome();
    return;
  }
  delay(100);

  const IPAddress ip = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;
  apMode = true;
  // Deliberately NO captive-portal DNS here: a captive portal would trap the
  // PWA inside iOS's sandboxed sign-in browser.
  startServer();
}

void TokenModeActivity::startServer() {
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

void TokenModeActivity::stopServer() { server.reset(); }

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

  // Confirm toggles STA <-> hotspot, but only from the idle screen — once a
  // card is showing, the buttons must not clobber it.
  if (!server->hasDisplayedImage() && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (apMode) {
      startWifiSelection(false);  // full picker: join a new network here
    } else {
      startHotspot();
    }
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

  const int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  if (apMode) {
    renderHotspotIdleScreen(startY);
  } else {
    renderStaIdleScreen(startY);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_EXIT), apMode ? tr(STR_JOIN_NETWORK) : tr(STR_CREATE_HOTSPOT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void TokenModeActivity::renderStaIdleScreen(int y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
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
}

void TokenModeActivity::renderHotspotIdleScreen(int y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int textX = (pageWidth - QR_SIZE) / 2 + QR_SIZE + metrics.verticalSpacing;

  // Step 1: join the hotspot (QR encodes SSID + WPA2 password)
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CONNECT_WIFI_HINT), true, EpdFontFamily::BOLD);
  y += height10 + metrics.verticalSpacing;

  const std::string wifiConfig = std::string("WIFI:T:WPA;S:") + AP_SSID + ";P:" + apPassword + ";;";
  QrUtils::drawQrCode(renderer, Rect{(pageWidth - QR_SIZE) / 2 - QR_SIZE / 2, y, QR_SIZE, QR_SIZE}, wifiConfig);
  renderer.drawText(UI_10_FONT_ID, textX - QR_SIZE / 2, y + QR_SIZE / 2 - height10, AP_SSID);
  const std::string passLine = std::string(tr(STR_PASSWORD)) + ": " + apPassword;
  renderer.drawText(SMALL_FONT_ID, textX - QR_SIZE / 2, y + QR_SIZE / 2 + 5, passLine.c_str());
  y += QR_SIZE + metrics.verticalSpacing * 2;

  // Step 2: open the app
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TOKEN_MODE_HINT), true, EpdFontFamily::BOLD);
  y += height10 + metrics.verticalSpacing;

  const std::string url = std::string("http://") + connectedIP + "/";
  QrUtils::drawQrCode(renderer, Rect{(pageWidth - QR_SIZE) / 2 - QR_SIZE / 2, y, QR_SIZE, QR_SIZE}, url);
  renderer.drawText(UI_10_FONT_ID, textX - QR_SIZE / 2, y + QR_SIZE / 2 - height10, url.c_str());
  const std::string mdnsUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + MDNS_HOSTNAME + ".local/";
  renderer.drawText(SMALL_FONT_ID, textX - QR_SIZE / 2, y + QR_SIZE / 2 + 5, mdnsUrl.c_str());
}
