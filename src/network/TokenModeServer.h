#pragma once

#include <WebServer.h>

#include <memory>

#include "TokenBmpDecoder.h"

class GfxRenderer;

/**
 * TokenModeServer — HTTP server for scoot-scoot token mode.
 *
 * Serves the scoot-scoot PWA (embedded, gzipped) and accepts full-screen
 * 1-bit BMP pushes on POST /display, blitting them straight into the
 * framebuffer via a streaming decoder (no image-sized allocation).
 *
 * Routes (see docs/PROTOCOL.md in the scoot-scoot repo):
 *   POST /display            image/bmp body, exact panel size, portrait
 *   GET  /api/token-status   JSON status (used by the app's health dot)
 *   GET  /*                  embedded PWA (SPA fallback to index.html)
 */
class TokenModeServer {
 public:
  explicit TokenModeServer(GfxRenderer& renderer) : renderer(renderer) {}
  ~TokenModeServer() { stop(); }

  void begin();
  void stop();
  void handleClient();
  bool isRunning() const { return running; }

  // millis() of the last handled HTTP request — drives the idle timeout.
  unsigned long lastRequestMillis() const { return lastRequestMs; }
  // True once a card image has been pushed (idle screen must not repaint over it).
  bool hasDisplayedImage() const { return displayedImage; }

  // Lets /api/token-status report which network the device is on.
  void setNetworkInfo(bool apMode, const char* ssid) {
    reportApMode = apMode;
    reportSsid = ssid;
  }

  // The app can reconfigure networking; the activity polls these after each
  // handleClient pump (the 200 goes out before the network drops).
  bool takePendingWifiJoin() {
    const bool value = pendingWifiJoin;
    pendingWifiJoin = false;
    return value;
  }
  bool takePendingHotspotSwitch() {
    const bool value = pendingHotspotSwitch;
    pendingHotspotSwitch = false;
    return value;
  }

 private:
  void handleDisplayPost();
  void handleDisplayRawBody();
  void handleStatus();
  void handleWifiScan();
  void handleWifiProvision();
  void handleModeSwitch();
  void handleNotFound();
  bool serveEmbeddedAsset(const char* path);
  void touch() { lastRequestMs = millis(); }
  void sendCors();

  static void sinkTrampoline(void* ctx, int x, int y);
  static void beforeFirstPixelTrampoline(void* ctx);

  GfxRenderer& renderer;
  std::unique_ptr<WebServer> server;
  std::unique_ptr<TokenBmpDecoder> decoder;
  bool running = false;
  bool displayedImage = false;
  unsigned long lastRequestMs = 0;
  bool reportApMode = false;
  std::string reportSsid;
  bool pendingWifiJoin = false;
  bool pendingHotspotSwitch = false;
};
