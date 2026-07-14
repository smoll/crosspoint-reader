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
constexpr const char* SETUP_LINK_BASE = "https://smoll.github.io";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_SIZE = 240;

// Stable per-device WPA2 password (>= 8 chars), derived from the factory MAC
// so it survives reflashes and can live on a sticker or the join QR.
std::string deriveApPassword() {
  const uint64_t mac = ESP.getEfuseMac();
  char pass[16];
  snprintf(pass, sizeof(pass), "scoot-%04x", static_cast<unsigned>((mac >> 24) & 0xFFFF));
  return pass;
}

// Minimal percent-encoding for URL fragment values (SSIDs can be anything).
std::string urlEncode(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
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
  // AP+STA so /api/wifi/scan can run while the hotspot is up.
  WiFi.mode(WIFI_AP_STA);
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
  server->setNetworkInfo(apMode, connectedSSID.c_str());

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

  // The app can reconfigure networking over HTTP; the 200 already went out,
  // so give TCP a moment to flush before tearing the network down.
  if (server->takePendingWifiJoin()) {
    delay(300);
    startWifiSelection(true);  // auto-joins the just-provisioned network; hotspot on failure
    return;
  }
  if (server->takePendingHotspotSwitch()) {
    delay(300);
    startHotspot();
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

// ONE QR for both modes: a universal link that opens the app (or the
// hosted fallback page) with connection info in the URL fragment.
// Format pinned by packages/core/src/setup-link.ts + its tests.
std::string TokenModeActivity::buildSetupUrl() const {
  std::string url = std::string(SETUP_LINK_BASE) + "/x3#v=1&m=" + (apMode ? "ap" : "sta");
  url += "&s=" + urlEncode(connectedSSID);
  if (apMode) url += "&p=" + urlEncode(apPassword);
  url += "&h=";
  url += apMode ? connectedIP : (std::string(MDNS_HOSTNAME) + ".local");
  return url;
}

void TokenModeActivity::renderStaIdleScreen(int y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TOKEN_SCAN_HINT), true, EpdFontFamily::BOLD);
  y += height10 + metrics.verticalSpacing * 2;

  QrUtils::drawQrCode(renderer, Rect{(pageWidth - QR_SIZE) / 2, y, QR_SIZE, QR_SIZE}, buildSetupUrl());
  y += QR_SIZE + metrics.verticalSpacing * 2;

  // Manual fallback for browsers: the device-served PWA.
  const std::string url = std::string("http://") + MDNS_HOSTNAME + ".local/";
  renderer.drawCenteredText(UI_10_FONT_ID, y, url.c_str(), true);
  y += height10 + 5;
  const std::string ipUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
  renderer.drawCenteredText(SMALL_FONT_ID, y, ipUrl.c_str(), true);
}

void TokenModeActivity::renderHotspotIdleScreen(int y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TOKEN_SCAN_HINT), true, EpdFontFamily::BOLD);
  y += height10 + metrics.verticalSpacing * 2;

  QrUtils::drawQrCode(renderer, Rect{(pageWidth - QR_SIZE) / 2, y, QR_SIZE, QR_SIZE}, buildSetupUrl());
  y += QR_SIZE + metrics.verticalSpacing * 2;

  // Manual fallback: join by hand, then open the device page.
  const std::string ssidLine = std::string(AP_SSID) + " · " + tr(STR_PASSWORD) + ": " + apPassword;
  renderer.drawCenteredText(UI_10_FONT_ID, y, ssidLine.c_str(), true);
  y += height10 + 5;
  const std::string url = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
  renderer.drawCenteredText(SMALL_FONT_ID, y, url.c_str(), true);
}
