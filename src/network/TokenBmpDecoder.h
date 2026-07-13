#pragma once

#include <cstddef>
#include <cstdint>

/**
 * TokenBmpDecoder — streaming decoder for the strict 1-bit BMP subset that
 * the scoot-scoot renderer produces (see docs/PROTOCOL.md in the parent
 * repo). Feeds arbitrary-size chunks (as delivered by WebServer raw upload)
 * and emits ink pixels through a callback, so a full-panel image decodes
 * with no image-sized allocation.
 *
 * Accepted format:
 * - BITMAPINFOHEADER (>= 40 bytes), BI_RGB, 1 bpp
 * - width/height exactly matching the expected panel size
 *   (bottom-up or top-down row order both handled)
 * - 2-entry palette; the darker entry is treated as ink
 *
 * Host-testable: no Arduino/ESP dependencies (see test/token_bmp_decoder).
 */
class TokenBmpDecoder {
 public:
  enum class Status : uint8_t {
    NeedMoreData,  // keep feeding
    Done,          // full image decoded
    Error,         // see errorMessage()
  };

  // Called once per INK pixel (callers pre-clear the target to white before
  // the first pixel; see onBeforeFirstPixel). Plain function pointer + ctx
  // per project convention (no std::function).
  using PixelSink = void (*)(void* ctx, int x, int y);
  // Called once, after the header validates and before the first pixel.
  using BeforeFirstPixelHook = void (*)(void* ctx);

  TokenBmpDecoder(int expectedWidth, int expectedHeight, PixelSink sink, BeforeFirstPixelHook beforeFirst, void* ctx)
      : expectedWidth(expectedWidth), expectedHeight(expectedHeight), sink(sink), beforeFirst(beforeFirst), ctx(ctx) {}

  Status feed(const uint8_t* data, size_t len);

  Status status() const { return currentStatus; }
  const char* errorMessage() const { return error; }

  static constexpr size_t MAX_HEADER_BYTES = 1024;

 private:
  Status fail(const char* message) {
    error = message;
    currentStatus = Status::Error;
    return currentStatus;
  }
  bool parseHeader();
  void emitPixelByte(uint8_t byte, size_t pixelByteIndex);

  const int expectedWidth;
  const int expectedHeight;
  const PixelSink sink;
  const BeforeFirstPixelHook beforeFirst;
  void* const ctx;

  Status currentStatus = Status::NeedMoreData;
  const char* error = "";

  // Header accumulation (headers + palette are tiny; pixel data is streamed)
  uint8_t header[MAX_HEADER_BYTES] = {};
  size_t headerBytes = 0;
  bool headerParsed = false;
  bool firstPixelSeen = false;

  // Parsed geometry
  uint32_t pixelDataOffset = 0;
  size_t rowBytes = 0;
  bool bottomUp = true;
  uint8_t inkBit = 0;  // bit value that maps to ink (from palette luminance)

  size_t totalBytesSeen = 0;
  size_t pixelBytesSeen = 0;
};
