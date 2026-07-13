#include "TokenModeServer.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "TokenWebAssets.embedded.h"

namespace {
constexpr const char* TAG = "TOKENSRV";
constexpr uint16_t HTTP_PORT = 80;
// A 528x792 1-bit BMP is ~53 KB; anything much larger is not ours.
constexpr size_t MAX_DISPLAY_BODY = 256 * 1024;
}  // namespace

void TokenModeServer::begin() {
  if (running) return;

  server = makeUniqueNoThrow<WebServer>(HTTP_PORT);
  if (!server) {
    LOG_ERR(TAG, "OOM: WebServer");
    return;
  }

  // POST /display: raw body streamed through TokenBmpDecoder into the framebuffer.
  server->on("/display", HTTP_POST, [this] { handleDisplayPost(); }, [this] { handleDisplayRawBody(); });
  // CORS preflight for pushes from a dev origin (production is same-origin).
  server->on("/display", HTTP_OPTIONS, [this] {
    touch();
    sendCors();
    server->sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server->send(204, "text/plain", "");
  });
  server->on("/api/token-status", HTTP_GET, [this] { handleStatus(); });
  server->onNotFound([this] { handleNotFound(); });

  server->begin();
  running = true;
  touch();
  LOG_DBG(TAG, "Token mode server listening on port %u", HTTP_PORT);
}

void TokenModeServer::stop() {
  if (!running) return;
  server->stop();
  server.reset();
  decoder.reset();
  running = false;
  LOG_DBG(TAG, "Token mode server stopped");
}

void TokenModeServer::handleClient() {
  if (running) server->handleClient();
}

void TokenModeServer::sendCors() { server->sendHeader("Access-Control-Allow-Origin", "*"); }

void TokenModeServer::sinkTrampoline(void* ctx, const int x, const int y) {
  static_cast<TokenModeServer*>(ctx)->renderer.drawPixel(x, y, true);
}

void TokenModeServer::beforeFirstPixelTrampoline(void* ctx) {
  // Header validated: whiten the framebuffer so unset bits render as paper.
  static_cast<TokenModeServer*>(ctx)->renderer.clearScreen();
}

// Upload/raw callback: called repeatedly with body chunks as they arrive.
void TokenModeServer::handleDisplayRawBody() {
  HTTPRaw& raw = server->raw();
  if (raw.status == RAW_START) {
    touch();
    if (server->clientContentLength() > MAX_DISPLAY_BODY) {
      // Decoder absent signals "rejected"; the POST handler reports it.
      decoder.reset();
      LOG_ERR(TAG, "Rejected /display body of %u bytes", (unsigned)server->clientContentLength());
      return;
    }
    decoder = makeUniqueNoThrow<TokenBmpDecoder>(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                                 &TokenModeServer::sinkTrampoline,
                                                 &TokenModeServer::beforeFirstPixelTrampoline, this);
    if (!decoder) LOG_ERR(TAG, "OOM: TokenBmpDecoder");
  } else if (raw.status == RAW_WRITE) {
    if (decoder && decoder->status() == TokenBmpDecoder::Status::NeedMoreData) {
      decoder->feed(raw.buf, raw.currentSize);
    }
  }
  // RAW_END / RAW_ABORTED: nothing to flush; handleDisplayPost() finishes up.
}

void TokenModeServer::handleDisplayPost() {
  touch();
  sendCors();

  if (!decoder) {
    char msg[96];
    snprintf(msg, sizeof(msg), "body too large or out of memory (max %u bytes)", (unsigned)MAX_DISPLAY_BODY);
    server->send(413, "text/plain", msg);
    return;
  }

  const auto status = decoder->status();
  if (status != TokenBmpDecoder::Status::Done) {
    char msg[160];
    const char* detail = status == TokenBmpDecoder::Status::Error ? decoder->errorMessage() : "truncated BMP body";
    snprintf(msg, sizeof(msg), "%s — expected an uncompressed 1-bit BMP, exactly %dx%d (portrait)", detail,
             renderer.getScreenWidth(), renderer.getScreenHeight());
    decoder.reset();
    LOG_ERR(TAG, "/display rejected: %s", msg);
    server->send(400, "text/plain", msg);
    return;
  }
  decoder.reset();

  // Full refresh clears ghosting from the previous card; ~2 s. The 200 is
  // sent after the refresh so the app's success state means "on screen".
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  displayedImage = true;
  LOG_DBG(TAG, "Card pushed to display");
  server->send(200, "application/json", "{\"ok\":true}");
}

void TokenModeServer::handleStatus() {
  touch();
  sendCors();
  char body[128];
  snprintf(body, sizeof(body), "{\"mode\":\"token\",\"width\":%d,\"height\":%d,\"freeHeap\":%u}",
           renderer.getScreenWidth(), renderer.getScreenHeight(), (unsigned)ESP.getFreeHeap());
  server->send(200, "application/json", body);
}

bool TokenModeServer::serveEmbeddedAsset(const char* path) {
  for (size_t i = 0; i < TOKEN_WEB_ASSETS_COUNT; i++) {
    const TokenWebAsset& asset = TOKEN_WEB_ASSETS[i];
    if (strcmp(path, asset.path) != 0) continue;
    if (asset.gzipped) server->sendHeader("Content-Encoding", "gzip");
    // Hashed asset names are immutable; everything else must revalidate so
    // a firmware update ships the new app immediately.
    if (strncmp(path, "/assets/", 8) == 0) {
      server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    } else {
      server->sendHeader("Cache-Control", "no-cache");
    }
    server->send_P(200, asset.contentType, reinterpret_cast<const char*>(asset.data), asset.size);
    return true;
  }
  return false;
}

void TokenModeServer::handleNotFound() {
  touch();
  const String& uri = server->uri();
  if (server->method() == HTTP_GET) {
    if (serveEmbeddedAsset(uri.c_str())) return;
    // SPA fallback: deep links like /c/<scryfall-id> get index.html.
    if (uri.indexOf('.') < 0 && serveEmbeddedAsset("/index.html")) return;
  }
  server->send(404, "text/plain", "not found");
}
