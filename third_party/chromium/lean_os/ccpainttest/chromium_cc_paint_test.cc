// cc's paint layer, on this machine, on the path a browser with no GPU takes.
//
// M157 got Skia, which is what a browser paints WITH. //cc/paint is the layer
// directly above it and the one Blink actually produces: a DisplayItemList is
// the recorded form of a paint, an rtree over it decides which of those
// operations a given tile needs, and rastering replays the chosen ones into
// an SkCanvas. On a machine with a GPU that replay happens on the GPU. There
// is none here, so what runs is the software path Chromium maintains for
// every platform for the case where GPU compositing is off, and cc says in
// its own source what that means:
//
//   cc/trees/layer_tree_host_impl.cc
//     // No context provider means software raster + compositing.
//     // Software compositor always uses BGRA 8888 format for tiles.
//
// BGRA 8888 premultiplied is byte-for-byte this compositor's own word layout,
// which is the third time that has paid - libnsfb in M113, Skia in M157, and
// cc's tiles here. Check 9 reads the raw bytes rather than trusting it.
//
// The canvas arithmetic checks 3, 4 and 5 perform is not invented here. It is
// what cc::RasterSource::PlaybackToCanvas does to fill a tile:
//
//   raster_canvas->translate(-canvas_bitmap_rect.x(), -canvas_bitmap_rect.y());
//   raster_canvas->clipRect(SkRect::Make(raster_bounds));
//   raster_canvas->scale(raster_transform.scale().x() / ..., ...);
//
// Every check is a PIXEL VALUE this program works out for itself, for the
// reason M40 wrote down and M157 repeated: a rasteriser that links and
// produces a blank bitmap passes every test that only asks whether the calls
// returned. The ones that matter most are 3 and 4 - a tile at an offset and a
// tile at a raster scale - because that arithmetic is where a compositor goes
// wrong in a way nobody notices until content is half a tile out.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "cc/paint/display_item_list.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_op.h"
#include "cc/paint/paint_op_buffer.h"
#include "cc/paint/solid_color_analyzer.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkClipOp.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumcc: %s\n", what);
  } else {
    std::printf("chromiumcc: FAIL %s\n", what);
    ++failures;
  }
}

constexpr int kLayerSide = 64;

// Opaque and far apart in every channel, so a pixel that is the wrong one of
// them cannot be a rounding difference - it is content in the wrong place.
constexpr SkColor kGround = SkColorSetARGB(0xFF, 0x10, 0x20, 0x30);
constexpr SkColor kNear = SkColorSetARGB(0xFF, 0x80, 0xC0, 0x40);
constexpr SkColor kFar = SkColorSetARGB(0xFF, 0xE0, 0xA0, 0x20);
constexpr SkColor kSentinel = SkColorSetARGB(0xFF, 0xFF, 0x00, 0xFF);

// The near rectangle sits inside the first 32x32 tile and the far one inside
// the second, which is what makes check 3 able to tell them apart.
const gfx::Rect kWholeLayer(0, 0, kLayerSide, kLayerSide);
const gfx::Rect kNearRect(10, 10, 20, 20);
const gfx::Rect kFarRect(40, 40, 10, 10);

void PushRect(cc::DisplayItemList* list, const gfx::Rect& rect, SkColor colour) {
  list->StartPaint();
  cc::PaintFlags flags;
  flags.setColor(colour);
  list->push<cc::DrawRectOp>(gfx::RectToSkRect(rect), flags);
  list->EndPaintOfUnpaired(rect);
}

scoped_refptr<cc::DisplayItemList> RecordThreeRectangles() {
  auto list = base::MakeRefCounted<cc::DisplayItemList>();
  PushRect(list.get(), kWholeLayer, kGround);
  PushRect(list.get(), kNearRect, kNear);
  PushRect(list.get(), kFarRect, kFar);
  list->Finalize();
  return list;
}

scoped_refptr<cc::DisplayItemList> RecordOneColour() {
  auto list = base::MakeRefCounted<cc::DisplayItemList>();
  PushRect(list.get(), kWholeLayer, kNear);
  list->Finalize();
  return list;
}

// The tile every check below rasters into: 8-bit BGRA, premultiplied, which
// is what cc chooses for a software compositor's tiles and what this
// compositor's back buffer already is.
SkBitmap MakeTile(int side, SkColor prefill) {
  SkBitmap tile;
  tile.allocPixels(SkImageInfo::Make(side, side, kBGRA_8888_SkColorType,
                                     kPremul_SkAlphaType));
  tile.eraseColor(prefill);
  return tile;
}

SkColor At(const SkBitmap& tile, int x, int y) {
  return tile.getColor(x, y);
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf(
      "chromiumcc: Chromium's //cc/paint, built for this machine and running "
      "on it - the software raster path, with no GPU stack in the build\n");

  scoped_refptr<cc::DisplayItemList> list = RecordThreeRectangles();

  // 1. Blink's half of the contract: three paint operations recorded into a
  //    list that is finalised and can be replayed. Nothing below can mean
  //    anything if this is not true, so it is graded on its own rather than
  //    assumed by the next check.
  Check("a recorded display list holds the three operations it was given",
        list && list->TotalOpCount() == 3);
  if (!list) {
    std::printf("chromiumcc: FAILED - nothing was recorded, %d checks\n",
                checks);
    return 1;
  }

  // 2. The whole layer into one tile, at identity. The interesting values are
  //    not the middles of the rectangles but their EDGES: 9 against 10 and 29
  //    against 30 are the difference between a rectangle drawn where it was
  //    asked for and one drawn a pixel out, and only the edges can tell.
  {
    SkBitmap tile = MakeTile(kLayerSide, kSentinel);
    SkCanvas canvas(tile);
    list->Raster(&canvas);
    bool edges = At(tile, 5, 5) == kGround && At(tile, 9, 9) == kGround &&
                 At(tile, 10, 10) == kNear && At(tile, 29, 29) == kNear &&
                 At(tile, 30, 30) == kGround && At(tile, 45, 45) == kFar;
    Check("a whole layer rastered at identity puts both rectangles on exactly "
          "the pixels they were asked for",
          edges);
    if (!edges) {
      std::printf("chromiumcc:   (9,9)=%08x (10,10)=%08x (29,29)=%08x "
                  "(30,30)=%08x (45,45)=%08x\n",
                  At(tile, 9, 9), At(tile, 10, 10), At(tile, 29, 29),
                  At(tile, 30, 30), At(tile, 45, 45));
    }
  }

  // 3. Two 32x32 tiles of the same layer, filled the way RasterSource fills
  //    one: clip to the tile, translate by its origin, replay. This is the
  //    arithmetic a compositor actually does, and the failure it hides is
  //    content half a tile out - the near rectangle belongs only to the first
  //    tile and the far one only to the second, each at a DIFFERENT coordinate
  //    from the one it was recorded at. Grading both catches a translation
  //    applied twice as well as one not applied at all.
  {
    SkBitmap first = MakeTile(32, kSentinel);
    {
      SkCanvas canvas(first);
      canvas.clipRect(SkRect::MakeWH(32, 32));
      canvas.translate(0, 0);
      list->Raster(&canvas);
    }
    SkBitmap second = MakeTile(32, kSentinel);
    {
      SkCanvas canvas(second);
      canvas.clipRect(SkRect::MakeWH(32, 32));
      canvas.translate(-32, -32);
      list->Raster(&canvas);
    }

    bool first_ok = At(first, 15, 15) == kNear && At(first, 5, 5) == kGround;
    // The far rectangle is at layer (40,40); in the second tile that is
    // bitmap (8,8), and layer (33,33) - bitmap (1,1) - is still ground.
    bool second_ok = At(second, 8, 8) == kFar && At(second, 1, 1) == kGround &&
                     At(second, 18, 18) == kGround;
    Check("a tile at an offset gets the content that belongs to it, at the "
          "coordinate the offset puts it",
          first_ok && second_ok);
    if (!(first_ok && second_ok)) {
      std::printf("chromiumcc:   tile0(15,15)=%08x tile1(8,8)=%08x "
                  "tile1(1,1)=%08x\n",
                  At(first, 15, 15), At(second, 8, 8), At(second, 1, 1));
    }
  }

  // 4. The same layer at a raster scale of two. A browser rasters at the scale
  //    the page is being shown at rather than at the scale it was recorded at,
  //    so this is not an optional path - and the near rectangle's edges move
  //    from 10..30 to 20..60, which is checkable to the pixel.
  {
    SkBitmap tile = MakeTile(kLayerSide, kSentinel);
    SkCanvas canvas(tile);
    canvas.scale(2.0f, 2.0f);
    list->Raster(&canvas);
    bool scaled = At(tile, 19, 19) == kGround && At(tile, 20, 20) == kNear &&
                  At(tile, 59, 59) == kNear && At(tile, 60, 60) == kGround;
    Check("at a raster scale of two the rectangle's edges land on 20 and 60 "
          "rather than on 10 and 30",
          scaled);
    if (!scaled) {
      std::printf("chromiumcc:   (19,19)=%08x (20,20)=%08x (59,59)=%08x "
                  "(60,60)=%08x\n",
                  At(tile, 19, 19), At(tile, 20, 20), At(tile, 59, 59),
                  At(tile, 60, 60));
    }
  }

  // 5. Partial raster: replaying into only the part of a tile that was
  //    invalidated, which is what makes a scrolling page affordable. The check
  //    is the half that a test of "did it draw" would miss - that everything
  //    OUTSIDE the clip was left alone, still holding the sentinel this
  //    program put there.
  {
    SkBitmap tile = MakeTile(kLayerSide, kSentinel);
    SkCanvas canvas(tile);
    canvas.clipRect(SkRect::MakeWH(20, 20));
    list->Raster(&canvas);
    bool inside = At(tile, 5, 5) == kGround && At(tile, 15, 15) == kNear;
    bool outside = At(tile, 25, 25) == kSentinel &&
                   At(tile, 45, 45) == kSentinel;
    Check("a partial raster draws inside the clip and leaves every pixel "
          "outside it untouched",
          inside && outside);
    if (!(inside && outside)) {
      std::printf("chromiumcc:   in(15,15)=%08x out(25,25)=%08x "
                  "out(45,45)=%08x\n",
                  At(tile, 15, 15), At(tile, 25, 25), At(tile, 45, 45));
    }
  }

  // 6. The rtree, which is the reason a tile of a large page costs what the
  //    tile covers rather than what the page contains. It is the one check
  //    here that is not a pixel, and it is worth having: a culling structure
  //    that returned everything would produce identical pixels everywhere
  //    above and make a real page unaffordable.
  {
    SkBitmap tile = MakeTile(kLayerSide, kSentinel);
    SkCanvas whole(tile);
    size_t all = list->OffsetsOfOpsToRaster(&whole).size();

    SkBitmap corner = MakeTile(16, kSentinel);
    SkCanvas clipped(corner);
    clipped.clipRect(SkRect::MakeWH(8, 8));
    size_t few = list->OffsetsOfOpsToRaster(&clipped).size();

    Check("the rtree returns every operation for the whole layer and fewer "
          "for a corner of it",
          all == 3 && few < all && few >= 1);
    if (!(all == 3 && few < all && few >= 1)) {
      std::printf("chromiumcc:   whole layer %zu ops, corner %zu ops\n", all,
                  few);
    }
  }

  // 7. The solid-colour analysis, which is how a compositor avoids allocating
  //    a tile for a region that is one colour. Getting this WRONG in the
  //    optimistic direction is how a page loses its content, so both
  //    directions are graded: a solid recording says so and names the colour,
  //    and the three-rectangle recording refuses.
  {
    scoped_refptr<cc::DisplayItemList> solid = RecordOneColour();
    std::optional<SkColor4f> colour = cc::SolidColorAnalyzer::
        DetermineIfSolidColor(solid->paint_op_buffer(), kWholeLayer, 4);
    Check("a recording of one colour is analysed as solid, and as that colour",
          colour.has_value() && colour->toSkColor() == kNear);

    std::optional<SkColor4f> other = cc::SolidColorAnalyzer::
        DetermineIfSolidColor(list->paint_op_buffer(), kWholeLayer, 4);
    Check("a recording with three rectangles in it is not analysed as solid",
          !other.has_value());
  }

  // 8. And the byte order underneath all of it, read rather than trusted. A
  //    tile whose pixels are correct through getColor() and wrong in memory
  //    would pass every check above and arrive on the screen with its red and
  //    blue exchanged - which is exactly the bug a zero-copy blit into this
  //    compositor cannot survive and a copy through a converter would hide.
  {
    SkBitmap tile = MakeTile(kLayerSide, kSentinel);
    SkCanvas canvas(tile);
    list->Raster(&canvas);
    const uint8_t* pixel = static_cast<const uint8_t*>(tile.getAddr(15, 15));
    bool bgra = pixel[0] == SkColorGetB(kNear) &&
                pixel[1] == SkColorGetG(kNear) &&
                pixel[2] == SkColorGetR(kNear) &&
                pixel[3] == SkColorGetA(kNear);
    Check("and in memory that pixel is B,G,R,A in that order - the word this "
          "compositor blits without touching a byte",
          bgra);
    if (!bgra) {
      std::printf("chromiumcc:   bytes were %02x %02x %02x %02x, wanted "
                  "%02x %02x %02x %02x\n",
                  pixel[0], pixel[1], pixel[2], pixel[3], SkColorGetB(kNear),
                  SkColorGetG(kNear), SkColorGetR(kNear), SkColorGetA(kNear));
    }
  }

  if (failures != 0) {
    std::printf("chromiumcc: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m158] cc rasters on this machine: %d checks, the recorded form of a "
      "paint that Blink produces and Skia consumes, replayed on the software "
      "path, with no GL, no Vulkan and no GPU stack anywhere in the build.\n",
      checks);
  std::printf("chromiumcc: done\n");
  return 0;
}
