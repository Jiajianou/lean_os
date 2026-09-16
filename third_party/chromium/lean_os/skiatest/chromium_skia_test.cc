// Skia, on this machine.
//
// Skia is what a browser draws with, and it is the half of Blink that V8 is
// not: M155 and M156 got the scripts running, and this is the painting. Every
// check below is a PIXEL VALUE this program works out for itself and Skia has
// to agree with - a colour at a coordinate, a coverage at an edge, a byte
// after a round trip through a codec. That is deliberate: a rasteriser that
// links and produces a blank bitmap passes every test that only asks whether
// the calls returned, and this project learned in M40 that the only honest
// grade for something that draws is the pixels it drew.
//
// It reports its own results and exits non-zero on any failure, the way
// /bin/chromiumv8 and /bin/chromiumbase do, because the boot self-test that
// runs it has no other way to tell what went wrong.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "third_party/skia/include/codec/SkCodec.h"
// The RUST spellings, and not because they are exotic: this build defines
// SK_CODEC_ENCODES_PNG_WITH_RUST and SK_CODEC_DECODES_PNG_WITH_RUST, so the
// Rust codecs are the PNG implementation Chromium ships here and libpng's
// entry points are not compiled at all. Asking for SkPngEncoder would be
// asking for the one that is not in this build.
#include "third_party/skia/include/codec/SkPngRustDecoder.h"
#include "third_party/skia/include/core/SkAlphaType.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/encode/SkPngRustEncoder.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumskia: %s\n", what);
  } else {
    std::printf("chromiumskia: FAIL %s\n", what);
    ++failures;
  }
}

constexpr int kWidth = 64;
constexpr int kHeight = 64;

// One bitmap every check draws into, in the layout this compositor uses:
// 8 bits a channel, premultiplied, BGRA on a little-endian machine - which
// is the same word order kernel/drivers' framebuffer wants, so a Skia bitmap
// could be blitted to this screen without touching a byte.
SkBitmap MakeBitmap() {
  SkBitmap bitmap;
  bitmap.allocPixels(
      SkImageInfo::Make(kWidth, kHeight, kBGRA_8888_SkColorType,
                        kPremul_SkAlphaType));
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  return bitmap;
}

SkColor At(const SkBitmap& bitmap, int x, int y) {
  return bitmap.getColor(x, y);
}

const char* NameOf(SkColor color) {
  static char text[64];
  std::snprintf(text, sizeof(text), "a=%u r=%u g=%u b=%u", SkColorGetA(color),
                SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
  return text;
}

void Fail(const char* what, SkColor got, SkColor wanted) {
  std::printf("chromiumskia:   at issue: wanted %s", NameOf(wanted));
  std::printf(", got %s\n", NameOf(got));
  Check(what, false);
}

void CheckPixel(const char* what, const SkBitmap& bitmap, int x, int y,
                SkColor wanted) {
  SkColor got = At(bitmap, x, y);
  if (got == wanted) {
    Check(what, true);
  } else {
    Fail(what, got, wanted);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  // 1. A surface, and a fill that reaches every pixel of it. A rasteriser
  //    that draws nothing at all fails here and nowhere else.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SkColorSetARGB(0xFF, 0x20, 0x40, 0x80));
    bool uniform = true;
    for (int y = 0; y < kHeight && uniform; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        if (At(bitmap, x, y) != SkColorSetARGB(0xFF, 0x20, 0x40, 0x80)) {
          uniform = false;
          break;
        }
      }
    }
    Check("a cleared surface is that colour in all 4096 of its pixels",
          uniform);
  }

  // 2. A rectangle lands exactly where it was told. The corners are the
  //    check: one pixel inside is the fill and one pixel outside is still
  //    the ground, which is what an off-by-one in either direction breaks.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    paint.setAntiAlias(false);
    canvas.drawRect(SkRect::MakeLTRB(16, 16, 48, 48), paint);

    bool exact = At(bitmap, 16, 16) == SK_ColorRED &&
                 At(bitmap, 47, 47) == SK_ColorRED &&
                 At(bitmap, 15, 16) == SK_ColorBLACK &&
                 At(bitmap, 16, 15) == SK_ColorBLACK &&
                 At(bitmap, 48, 47) == SK_ColorBLACK &&
                 At(bitmap, 47, 48) == SK_ColorBLACK;
    Check("a filled rectangle covers exactly the pixels it was given", exact);
  }

  // 3. Alpha blending, as a number rather than an impression. Half-opaque
  //    white over black is 0x7F or 0x80 depending on how the rounding goes,
  //    and nothing else is a blend.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    SkPaint paint;
    paint.setColor(SkColorSetARGB(0x80, 0xFF, 0xFF, 0xFF));
    paint.setAntiAlias(false);
    canvas.drawRect(SkRect::MakeLTRB(0, 0, kWidth, kHeight), paint);

    SkColor got = At(bitmap, 32, 32);
    unsigned red = SkColorGetR(got);
    bool blended = SkColorGetA(got) == 0xFF && red >= 0x7E && red <= 0x81 &&
                   SkColorGetG(got) == red && SkColorGetB(got) == red;
    if (blended) {
      Check("half-opaque white over black blends to the midpoint", true);
    } else {
      Fail("half-opaque white over black blends to the midpoint", got,
           SkColorSetARGB(0xFF, 0x80, 0x80, 0x80));
    }
  }

  // 4. Anti-aliasing, which is the thing a rasteriser is actually FOR. The
  //    centre of the circle must be solid, a corner of the bitmap must be
  //    untouched, and the edge must have at least one pixel that is NEITHER
  //    - a partly covered one. This project's own icon generator refuses a
  //    face that fails exactly this test.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    paint.setAntiAlias(true);
    canvas.drawCircle(32, 32, 20, paint);

    int partial = 0;
    for (int x = 0; x < kWidth; ++x) {
      for (int y = 0; y < kHeight; ++y) {
        unsigned red = SkColorGetR(At(bitmap, x, y));
        if (red != 0 && red != 0xFF) {
          ++partial;
        }
      }
    }
    bool shaped = At(bitmap, 32, 32) == SK_ColorWHITE &&
                  At(bitmap, 0, 0) == SK_ColorBLACK && partial > 32;
    std::printf("chromiumskia:   %d partly covered pixels on the edge\n",
                partial);
    Check("an anti-aliased circle is solid inside, clear outside, and soft "
          "at the edge", shaped);
  }

  // 5. A path with a hole in it, decided by the fill rule rather than by
  //    drawing order - two squares wound the same way, even-odd, so the
  //    middle comes out empty.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    SkPathBuilder builder;
    builder.setFillType(SkPathFillType::kEvenOdd);
    builder.addRect(SkRect::MakeLTRB(8, 8, 56, 56));
    builder.addRect(SkRect::MakeLTRB(24, 24, 40, 40));
    SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    paint.setAntiAlias(false);
    canvas.drawPath(builder.detach(), paint);

    bool holed = At(bitmap, 12, 12) == SK_ColorGREEN &&
                 At(bitmap, 32, 32) == SK_ColorBLACK &&
                 At(bitmap, 52, 52) == SK_ColorGREEN;
    Check("an even-odd path leaves the hole its winding asks for", holed);
  }

  // 6. A clip is a refusal. Drawing a rectangle that covers everything, with
  //    a clip over a quarter of it, must change one quarter and nothing else.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    canvas.save();
    canvas.clipRect(SkRect::MakeLTRB(0, 0, 32, 32));
    SkPaint paint;
    paint.setColor(SK_ColorBLUE);
    canvas.drawRect(SkRect::MakeLTRB(0, 0, kWidth, kHeight), paint);
    canvas.restore();

    bool clipped = At(bitmap, 31, 31) == SK_ColorBLUE &&
                   At(bitmap, 32, 31) == SK_ColorBLACK &&
                   At(bitmap, 31, 32) == SK_ColorBLACK &&
                   At(bitmap, 63, 63) == SK_ColorBLACK;
    Check("a clip keeps a drawing inside it", clipped);
  }

  // 7. A transform. A 16x16 rectangle at the origin, scaled by two and moved
  //    to (16,16), has to cover (16,16) through (47,47) and nothing else -
  //    which is arithmetic this program does and Skia's matrix has to match.
  {
    SkBitmap bitmap = MakeBitmap();
    SkCanvas canvas(bitmap);
    canvas.clear(SK_ColorBLACK);
    canvas.save();
    canvas.translate(16, 16);
    canvas.scale(2, 2);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    paint.setAntiAlias(false);
    canvas.drawRect(SkRect::MakeWH(16, 16), paint);
    canvas.restore();

    bool placed = At(bitmap, 16, 16) == SK_ColorRED &&
                  At(bitmap, 47, 47) == SK_ColorRED &&
                  At(bitmap, 15, 16) == SK_ColorBLACK &&
                  At(bitmap, 48, 48) == SK_ColorBLACK;
    Check("a scaled and translated rectangle lands where the matrix says",
          placed);
  }

  // 8. PNG, out and back. This is the one check that is not only Skia: the
  //    encoder and decoder Chromium ships are written in RUST - the `png`
  //    crate behind a cxx bridge - which is the same Rust half of the build
  //    that fontconfig's fontations backend comes from, and the reason the
  //    crates.io libc crate had to learn this target exists at all. A
  //    byte-exact round trip here says that half works.
  {
    SkBitmap source = MakeBitmap();
    SkCanvas canvas(source);
    canvas.clear(SK_ColorBLACK);
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setColor(SK_ColorRED);
    canvas.drawRect(SkRect::MakeLTRB(0, 0, 32, 32), paint);
    paint.setColor(SK_ColorGREEN);
    canvas.drawRect(SkRect::MakeLTRB(32, 0, 64, 32), paint);
    paint.setColor(SK_ColorBLUE);
    canvas.drawRect(SkRect::MakeLTRB(0, 32, 32, 64), paint);
    paint.setColor(SK_ColorWHITE);
    canvas.drawRect(SkRect::MakeLTRB(32, 32, 64, 64), paint);

    SkDynamicMemoryWStream out;
    bool encoded = SkPngRustEncoder::Encode(&out, source.pixmap(), {});
    sk_sp<SkData> png = out.detachAsData();
    std::printf("chromiumskia:   the encoder produced %zu bytes of PNG\n",
                png ? png->size() : 0u);
    Check("Skia's PNG encoder produced a file", encoded && png &&
                                                    png->size() > 100);

    bool signature = png && png->size() > 8 &&
                     std::memcmp(png->data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    Check("and it begins with PNG's own eight-byte signature", signature);

    SkBitmap back;
    bool decoded = false;
    if (png) {
      std::unique_ptr<SkCodec> codec = SkPngRustDecoder::Decode(
          SkMemoryStream::Make(png), nullptr);
      if (codec) {
        SkImageInfo info = codec->getInfo()
                               .makeColorType(kBGRA_8888_SkColorType)
                               .makeAlphaType(kPremul_SkAlphaType);
        back.allocPixels(info);
        decoded = codec->getPixels(info, back.getPixels(),
                                   back.rowBytes()) == SkCodec::kSuccess;
      }
    }
    Check("and the decoder read it back at the size it was written",
          decoded && back.width() == kWidth && back.height() == kHeight);

    int different = 0;
    if (decoded) {
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          if (At(source, x, y) != At(back, x, y)) {
            ++different;
          }
        }
      }
    }
    Check("and every one of the 4096 pixels came back the colour it went in "
          "as", decoded && different == 0);
    if (decoded && different != 0) {
      std::printf("chromiumskia:   %d pixels changed across the round trip\n",
                  different);
    }
  }

  if (failures != 0) {
    std::printf("chromiumskia: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m157] Skia rasterises on this machine: %d checks, the library a "
      "browser paints with, out of Chromium's own build, drawing into memory "
      "this compositor could blit without touching a byte.\n",
      checks);
  std::printf("chromiumskia: done\n");
  return 0;
}
