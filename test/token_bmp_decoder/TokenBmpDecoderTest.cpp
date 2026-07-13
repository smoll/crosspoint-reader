#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "src/network/TokenBmpDecoder.h"

namespace {

constexpr int W = 528;
constexpr int H = 792;

// Reference encoder mirroring packages/renderer/src/bmp.ts:
// BITMAPINFOHEADER, 1bpp, BI_RGB, bottom-up, palette[0]=black, palette[1]=white,
// bit 1 = white (paper). `ink` is row-major top-down, 1 = black.
std::vector<uint8_t> encodeBmp(int width, int height, const std::vector<uint8_t>& ink, bool topDown = false) {
  const size_t rowBytes = ((static_cast<size_t>(width) + 7) / 8 + 3) & ~static_cast<size_t>(3);
  const size_t headerSize = 14 + 40 + 8;
  const size_t fileSize = headerSize + rowBytes * height;
  std::vector<uint8_t> out(fileSize, 0);
  auto put16 = [&](size_t off, uint16_t v) { memcpy(&out[off], &v, 2); };
  auto put32 = [&](size_t off, uint32_t v) { memcpy(&out[off], &v, 4); };
  out[0] = 'B';
  out[1] = 'M';
  put32(2, static_cast<uint32_t>(fileSize));
  put32(10, static_cast<uint32_t>(headerSize));
  put32(14, 40);
  put32(18, static_cast<uint32_t>(width));
  put32(22, static_cast<uint32_t>(topDown ? -height : height));
  put16(26, 1);
  put16(28, 1);  // 1 bpp
  put32(30, 0);  // BI_RGB
  // palette: [0]=black, [1]=white (BGRA)
  out[58] = out[59] = out[60] = 0xff;
  for (int y = 0; y < height; y++) {
    const int srcRow = topDown ? y : height - 1 - y;
    const size_t off = headerSize + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; x++) {
      if (!ink[static_cast<size_t>(srcRow) * width + x]) {
        out[off + (x >> 3)] |= 0x80 >> (x & 7);  // paper -> bit 1
      }
    }
  }
  return out;
}

struct Capture {
  std::vector<uint8_t> pixels;
  int width = 0;
  bool cleared = false;
  int clearCount = 0;

  explicit Capture(int w, int h) : pixels(static_cast<size_t>(w) * h, 0), width(w) {}

  static void sink(void* ctx, int x, int y) {
    auto* c = static_cast<Capture*>(ctx);
    ASSERT_TRUE(c->cleared) << "pixel emitted before the clear hook";
    c->pixels[static_cast<size_t>(y) * c->width + x] = 1;
  }
  static void beforeFirst(void* ctx) {
    auto* c = static_cast<Capture*>(ctx);
    c->cleared = true;
    c->clearCount++;
  }
};

std::vector<uint8_t> checkerboard(int width, int height) {
  std::vector<uint8_t> ink(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      ink[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>((x + y) & 1);
    }
  }
  return ink;
}

TokenBmpDecoder::Status feedInChunks(TokenBmpDecoder& decoder, const std::vector<uint8_t>& data, size_t chunkSize) {
  TokenBmpDecoder::Status status = TokenBmpDecoder::Status::NeedMoreData;
  for (size_t off = 0; off < data.size(); off += chunkSize) {
    const size_t n = std::min(chunkSize, data.size() - off);
    status = decoder.feed(data.data() + off, n);
    if (status == TokenBmpDecoder::Status::Error) return status;
  }
  return status;
}

TEST(TokenBmpDecoder, DecodesFullPanelCheckerboardExactly) {
  const auto ink = checkerboard(W, H);
  const auto bmp = encodeBmp(W, H, ink);
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, bmp.size()), TokenBmpDecoder::Status::Done);
  EXPECT_EQ(cap.pixels, ink);
  EXPECT_EQ(cap.clearCount, 1);
}

TEST(TokenBmpDecoder, ChunkSizeDoesNotAffectOutput) {
  const auto ink = checkerboard(W, H);
  const auto bmp = encodeBmp(W, H, ink);
  // 1436 mirrors HTTP_RAW_BUFLEN; the odd sizes cross header boundaries.
  for (const size_t chunk : {7u, 61u, 1436u, 4096u}) {
    Capture cap(W, H);
    TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
    EXPECT_EQ(feedInChunks(decoder, bmp, chunk), TokenBmpDecoder::Status::Done) << "chunk=" << chunk;
    EXPECT_EQ(cap.pixels, ink) << "chunk=" << chunk;
  }
}

TEST(TokenBmpDecoder, HandlesTopDownRowOrder) {
  std::vector<uint8_t> ink(static_cast<size_t>(W) * H, 0);
  ink[0 * W + 0] = 1;                                 // top-left
  ink[static_cast<size_t>(H - 1) * W + (W - 1)] = 1;  // bottom-right
  for (const bool topDown : {false, true}) {
    const auto bmp = encodeBmp(W, H, ink, topDown);
    Capture cap(W, H);
    TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
    EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Done) << "topDown=" << topDown;
    EXPECT_EQ(cap.pixels, ink) << "topDown=" << topDown;
  }
}

TEST(TokenBmpDecoder, RejectsWrongDimensions) {
  const auto bmp = encodeBmp(100, 100, std::vector<uint8_t>(100 * 100, 0));
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Error);
  EXPECT_NE(strstr(decoder.errorMessage(), "size"), nullptr);
  EXPECT_FALSE(cap.cleared) << "framebuffer must not be touched on rejected input";
}

TEST(TokenBmpDecoder, RejectsNonBmpBody) {
  const uint8_t junk[] = "<html>definitely not a bmp</html>";
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(decoder.feed(junk, sizeof(junk)), TokenBmpDecoder::Status::Error);
  EXPECT_FALSE(cap.cleared);
}

TEST(TokenBmpDecoder, RejectsWrongBitDepth) {
  auto bmp = encodeBmp(W, H, std::vector<uint8_t>(static_cast<size_t>(W) * H, 0));
  bmp[28] = 8;  // claim 8 bpp
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Error);
  EXPECT_NE(strstr(decoder.errorMessage(), "1-bit"), nullptr);
}

TEST(TokenBmpDecoder, RejectsCompressedBmp) {
  auto bmp = encodeBmp(W, H, std::vector<uint8_t>(static_cast<size_t>(W) * H, 0));
  bmp[30] = 2;  // BI_RLE4
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Error);
}

TEST(TokenBmpDecoder, TruncatedBodyStaysIncomplete) {
  const auto bmp = encodeBmp(W, H, checkerboard(W, H));
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(decoder.feed(bmp.data(), bmp.size() / 2), TokenBmpDecoder::Status::NeedMoreData);
}

TEST(TokenBmpDecoder, InvertedPaletteStillMapsDarkToInk) {
  // Swap palette entries AND invert the pixel bits: same visual image.
  const auto ink = checkerboard(W, H);
  auto bmp = encodeBmp(W, H, ink);
  bmp[54] = bmp[55] = bmp[56] = 0xff;  // palette[0] = white
  bmp[58] = bmp[59] = bmp[60] = 0x00;  // palette[1] = black
  const size_t pixelOffset = 62;
  for (size_t i = pixelOffset; i < bmp.size(); i++) bmp[i] = static_cast<uint8_t>(~bmp[i]);
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Done);
  EXPECT_EQ(cap.pixels, ink);
}

TEST(TokenBmpDecoder, AllWhiteImageStillFiresClearHook) {
  const auto bmp = encodeBmp(W, H, std::vector<uint8_t>(static_cast<size_t>(W) * H, 0));
  Capture cap(W, H);
  TokenBmpDecoder decoder(W, H, &Capture::sink, &Capture::beforeFirst, &cap);
  EXPECT_EQ(feedInChunks(decoder, bmp, 1436), TokenBmpDecoder::Status::Done);
  EXPECT_TRUE(cap.cleared);
  EXPECT_TRUE(std::all_of(cap.pixels.begin(), cap.pixels.end(), [](uint8_t p) { return p == 0; }));
}

}  // namespace
