#include "TokenBmpDecoder.h"

#include <cstring>

namespace {
// Little-endian reads via memcpy — the ESP32-C3 (RISC-V) faults on unaligned
// multi-byte loads, so never cast into the header buffer.
uint16_t readU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
uint32_t readU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
int32_t readI32(const uint8_t* p) {
  int32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
}  // namespace

bool TokenBmpDecoder::parseHeader() {
  // Need the 14-byte file header + 4 bytes of info-header size first.
  if (headerBytes < 18) return false;
  if (header[0] != 'B' || header[1] != 'M') {
    fail("not a BMP (missing BM signature)");
    return false;
  }
  pixelDataOffset = readU32(header + 10);
  const uint32_t infoSize = readU32(header + 14);
  if (infoSize < 40) {
    fail("unsupported BMP header (BITMAPINFOHEADER required)");
    return false;
  }
  if (pixelDataOffset < 14 + infoSize + 8 || pixelDataOffset > MAX_HEADER_BYTES) {
    fail("unsupported pixel data offset");
    return false;
  }
  // Wait until everything before the pixel data has arrived.
  if (headerBytes < pixelDataOffset) return false;

  const int32_t width = readI32(header + 18);
  const int32_t rawHeight = readI32(header + 22);
  const uint16_t bpp = readU16(header + 28);
  const uint32_t compression = readU32(header + 30);
  const int32_t height = rawHeight < 0 ? -rawHeight : rawHeight;

  if (bpp != 1) {
    fail("expected a 1-bit BMP");
    return false;
  }
  if (compression != 0) {
    fail("compressed BMPs are not supported");
    return false;
  }
  if (width != expectedWidth || height != expectedHeight) {
    fail("image size must exactly match the panel (portrait)");
    return false;
  }

  bottomUp = rawHeight > 0;
  rowBytes = ((static_cast<size_t>(width) + 7) / 8 + 3) & ~static_cast<size_t>(3);

  // The darker palette entry is ink. Entries are BGRA at 14 + infoSize.
  const uint8_t* palette = header + 14 + infoSize;
  const int lum0 = palette[0] + palette[1] + palette[2];
  const int lum1 = palette[4] + palette[5] + palette[6];
  inkBit = lum0 <= lum1 ? 0 : 1;

  headerParsed = true;
  return true;
}

void TokenBmpDecoder::emitPixelByte(const uint8_t byte, const size_t pixelByteIndex) {
  const size_t row = pixelByteIndex / rowBytes;
  const size_t col = pixelByteIndex % rowBytes;
  if (row >= static_cast<size_t>(expectedHeight)) return;  // trailing padding
  const int baseX = static_cast<int>(col) * 8;
  if (baseX >= expectedWidth) return;  // row padding bytes
  const int y = bottomUp ? expectedHeight - 1 - static_cast<int>(row) : static_cast<int>(row);
  for (int bit = 0; bit < 8; bit++) {
    const int x = baseX + bit;
    if (x >= expectedWidth) break;
    const uint8_t value = (byte >> (7 - bit)) & 1;
    if (value == inkBit) {
      if (!firstPixelSeen) {
        firstPixelSeen = true;
        if (beforeFirst) beforeFirst(ctx);
      }
      sink(ctx, x, y);
    }
  }
}

TokenBmpDecoder::Status TokenBmpDecoder::feed(const uint8_t* data, size_t len) {
  if (currentStatus != Status::NeedMoreData) return currentStatus;

  size_t i = 0;
  // Accumulate header bytes until parseHeader succeeds.
  while (!headerParsed && i < len) {
    if (headerBytes >= MAX_HEADER_BYTES) return fail("header too large");
    header[headerBytes++] = data[i++];
    totalBytesSeen++;
    if (headerBytes >= 18 && headerBytes == pixelDataOffset) {
      if (!parseHeader() && currentStatus == Status::Error) return currentStatus;
    } else if (headerBytes == 18 && pixelDataOffset == 0) {
      // First chance to learn pixelDataOffset (parseHeader sets it or errors).
      if (!parseHeader() && currentStatus == Status::Error) return currentStatus;
    }
  }
  if (!headerParsed) return currentStatus;

  // Ensure the before-first-pixel hook fires even for an all-white image.
  if (!firstPixelSeen && pixelBytesSeen == 0) {
    firstPixelSeen = true;
    if (beforeFirst) beforeFirst(ctx);
  }

  const size_t pixelBytesTotal = rowBytes * static_cast<size_t>(expectedHeight);
  for (; i < len; i++) {
    totalBytesSeen++;
    if (pixelBytesSeen < pixelBytesTotal) {
      emitPixelByte(data[i], pixelBytesSeen);
    }
    pixelBytesSeen++;
  }

  if (pixelBytesSeen >= pixelBytesTotal) {
    currentStatus = Status::Done;
  }
  return currentStatus;
}
