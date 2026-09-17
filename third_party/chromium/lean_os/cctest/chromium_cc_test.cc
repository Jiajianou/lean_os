// //cc itself, on this machine: the layer path, the invalidation that makes
// partial repaint possible, and the tiling arithmetic underneath both.
//
// M158 got //cc/paint - the recorded form of a paint - and said in its own
// commit why it stopped there: //cc:cc reached //gpu/config, and a -k 0 build
// of it failed in 546 places of which 424 were a GPU stack in a build that
// had turned the GPU stack off. M159 removed that, so this is the rung above.
//
// What is new here is not rastering; M158 graded that. It is the half of cc
// between a LAYER and a raster source:
//
//   ContentLayerClient    the interface BLINK implements. Its one job is
//                         PaintContentsToDisplayList(), and everything cc
//                         does with a layer's content starts from it.
//   RecordingSource       holds the recording and the INVALIDATION. Its
//                         Update() returns false when nothing was dirtied,
//                         which is the entire mechanism by which a browser
//                         does not repaint a page on every frame.
//   RasterSource          the immutable snapshot a raster worker replays.
//   TilingData            which tile a pixel is in, and where that tile's
//                         bounds are - the arithmetic a compositor gets
//                         wrong in a way nobody notices until content is
//                         half a tile out.
//
// Every pixel check here is a value this program works out for itself, for
// the reason M40 set and M157 and M158 repeated: a compositor that links and
// produces a blank bitmap passes every test that only asks whether the calls
// returned. The invalidation checks are the sharper ones, because a cc that
// repainted everything every frame would produce identical pixels here and
// an unusable browser on a real page.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "cc/base/region.h"
#include "cc/base/tiling_data.h"
#include "cc/layers/content_layer_client.h"
#include "cc/layers/recording_source.h"
#include "cc/paint/display_item_list.h"
#include "cc/paint/paint_flags.h"
#include "cc/raster/raster_source.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/geometry/axis_transform2d.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/vector2d_f.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumcc2: %s\n", what);
  } else {
    std::printf("chromiumcc2: FAIL %s\n", what);
    ++failures;
  }
}

constexpr SkColor kBackground = SkColorSetARGB(0xFF, 0x20, 0x20, 0x20);
constexpr SkColor kNear = SkColorSetARGB(0xFF, 0xE0, 0x30, 0x10);
constexpr SkColor kFar = SkColorSetARGB(0xFF, 0x10, 0x90, 0xE0);

// The interface Blink implements. Two virtuals, and the whole of what cc
// knows about a layer's contents goes through the first of them.
class Painter : public cc::ContentLayerClient {
 public:
  explicit Painter(const gfx::Size& size) : size_(size) {}

  void set_solid(bool solid) { solid_ = solid; }
  int paints() const { return paints_; }

  scoped_refptr<cc::DisplayItemList> PaintContentsToDisplayList() override {
    ++paints_;
    auto list = base::MakeRefCounted<cc::DisplayItemList>();
    list->StartPaint();
    cc::PaintFlags background;
    background.setColor(kBackground);
    list->push<cc::DrawRectOp>(
        SkRect::MakeIWH(size_.width(), size_.height()), background);
    if (!solid_) {
      cc::PaintFlags near_flags;
      near_flags.setColor(kNear);
      list->push<cc::DrawRectOp>(SkRect::MakeLTRB(10, 10, 30, 30), near_flags);
      cc::PaintFlags far_flags;
      far_flags.setColor(kFar);
      list->push<cc::DrawRectOp>(SkRect::MakeLTRB(40, 40, 60, 60), far_flags);
    }
    list->EndPaintOfUnpaired(gfx::Rect(size_));
    list->Finalize();
    return list;
  }

  bool FillsBoundsCompletely() const override { return true; }

 private:
  gfx::Size size_;
  bool solid_ = false;
  int paints_ = 0;
};

SkBitmap MakeTile(int width, int height) {
  SkBitmap bitmap;
  bitmap.allocPixels(
      SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                        kPremul_SkAlphaType));
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  return bitmap;
}

SkColor PixelAt(const SkBitmap& bitmap, int x, int y) {
  return bitmap.getColor(x, y);
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf("chromiumcc2: starting\n");

  const gfx::Size layer_size(64, 64);
  Painter painter(layer_size);
  cc::RecordingSource recording;

  // 1. The layer path. A RecordingSource asked to update itself over a
  //    ContentLayerClient records the client's display list - which is the
  //    step between "Blink has painted" and "cc has something to raster".
  {
    cc::Region invalidation{gfx::Rect(layer_size)};
    bool updated = recording.Update(layer_size, 1.0f, painter, invalidation);
    Check("cc::RecordingSource::Update records a display list over the "
          "ContentLayerClient interface Blink implements",
          updated && recording.display_list() != nullptr &&
              painter.paints() == 1);
    Check("and the recording is the size of the layer",
          recording.size() == layer_size);
  }

  // 2. The invalidation, which is the whole reason a browser is affordable.
  //    An update with nothing dirty must NOT ask the client to paint again.
  //    A cc that repainted every frame would give identical pixels below and
  //    an unusable browser on a real page, so this is the check that no
  //    amount of pixel grading can replace.
  {
    cc::Region nothing;
    bool updated = recording.Update(layer_size, 1.0f, painter, nothing);
    Check("an update with an empty invalidation does not repaint - the "
          "client was asked once, not twice",
          !updated && painter.paints() == 1);
  }

  // 3. And a dirty rectangle does, and comes back as the region that was
  //    dirtied rather than as the whole layer.
  {
    recording.SetNeedsDisplayRect(gfx::Rect(10, 10, 20, 20));
    cc::Region invalidation;
    bool updated = recording.Update(layer_size, 1.0f, painter, invalidation);
    Check("a dirty rectangle does repaint, and the invalidation handed back "
          "is that rectangle rather than the whole layer",
          updated && painter.paints() == 2 &&
              invalidation.Contains(gfx::Rect(10, 10, 20, 20)) &&
              !invalidation.Contains(gfx::Rect(layer_size)));
  }

  scoped_refptr<cc::RasterSource> raster = recording.CreateRasterSource();
  Check("cc::RecordingSource::CreateRasterSource makes the immutable "
        "snapshot a raster worker replays",
        raster && raster->size() == layer_size);

  // 4. The whole layer at identity, graded on the rectangles' EDGES - 9
  //    against 10 and 29 against 30 - which is the only place a one-pixel
  //    error in any of this shows up at all.
  {
    SkBitmap whole = MakeTile(64, 64);
    SkCanvas canvas(whole);
    raster->PlaybackToCanvas(
        &canvas, layer_size, gfx::Rect(0, 0, 64, 64), gfx::Rect(0, 0, 64, 64),
        gfx::AxisTransform2d(), cc::RasterSource::PlaybackSettings());
    bool edges = PixelAt(whole, 9, 9) == kBackground &&
                 PixelAt(whole, 10, 10) == kNear &&
                 PixelAt(whole, 29, 29) == kNear &&
                 PixelAt(whole, 30, 30) == kBackground &&
                 PixelAt(whole, 40, 40) == kFar &&
                 PixelAt(whole, 59, 59) == kFar &&
                 PixelAt(whole, 60, 60) == kBackground;
    Check("a whole layer replayed at identity puts both rectangles' edges "
          "exactly where they were recorded", edges);
    if (!edges) {
      std::printf("chromiumcc2:   (9,9)=%08x (10,10)=%08x (29,29)=%08x "
                  "(30,30)=%08x (40,40)=%08x (60,60)=%08x\n",
                  PixelAt(whole, 9, 9), PixelAt(whole, 10, 10),
                  PixelAt(whole, 29, 29), PixelAt(whole, 30, 30),
                  PixelAt(whole, 40, 40), PixelAt(whole, 60, 60));
    }

    // And the bytes in memory are B,G,R,A - the word this compositor blits
    // without touching a byte. getColor() alone would not see an exchange.
    const uint8_t* pixel = static_cast<const uint8_t*>(whole.getAddr(15, 15));
    bool bgra = pixel[0] == SkColorGetB(kNear) &&
                pixel[1] == SkColorGetG(kNear) &&
                pixel[2] == SkColorGetR(kNear) &&
                pixel[3] == SkColorGetA(kNear);
    Check("and in memory that pixel is B,G,R,A in that order", bgra);
  }

  // 5. Two 32x32 tiles, filled the way cc fills one: the canvas is translated
  //    by the tile's origin so the layer's far rectangle lands at bitmap
  //    (8,8) rather than at layer (40,40). This is the arithmetic that puts
  //    content half a tile out when it is wrong.
  {
    SkBitmap first = MakeTile(32, 32);
    SkCanvas first_canvas(first);
    raster->PlaybackToCanvas(
        &first_canvas, layer_size, gfx::Rect(0, 0, 32, 32),
        gfx::Rect(0, 0, 32, 32), gfx::AxisTransform2d(),
        cc::RasterSource::PlaybackSettings());

    SkBitmap second = MakeTile(32, 32);
    SkCanvas second_canvas(second);
    raster->PlaybackToCanvas(
        &second_canvas, layer_size, gfx::Rect(32, 32, 32, 32),
        gfx::Rect(32, 32, 32, 32), gfx::AxisTransform2d(),
        cc::RasterSource::PlaybackSettings());

    Check("the near rectangle is in the first tile and not the second",
          PixelAt(first, 15, 15) == kNear && PixelAt(second, 15, 15) != kNear);
    Check("and the far rectangle is in the second tile at (8,8) - its layer "
          "position minus the tile's origin - and not in the first",
          PixelAt(second, 8, 8) == kFar && PixelAt(second, 27, 27) == kFar &&
              PixelAt(first, 8, 8) != kFar);
  }

  // 6. A raster scale of two. The near rectangle's edges move from 10..30 to
  //    20..60, which is what a device scale factor or a pinch zoom does.
  {
    SkBitmap scaled = MakeTile(128, 128);
    SkCanvas canvas(scaled);
    raster->PlaybackToCanvas(
        &canvas, raster->GetContentSize(gfx::Vector2dF(2.0f, 2.0f)),
        gfx::Rect(0, 0, 128, 128), gfx::Rect(0, 0, 128, 128),
        gfx::AxisTransform2d(2.0f, gfx::Vector2dF()),
        cc::RasterSource::PlaybackSettings());
    bool edges = PixelAt(scaled, 19, 19) == kBackground &&
                 PixelAt(scaled, 20, 20) == kNear &&
                 PixelAt(scaled, 59, 59) == kNear &&
                 PixelAt(scaled, 60, 60) == kBackground;
    Check("at a raster scale of two the near rectangle's edges move from "
          "10..30 to 20..60", edges);
    if (!edges) {
      std::printf("chromiumcc2:   (19,19)=%08x (20,20)=%08x (59,59)=%08x "
                  "(60,60)=%08x\n",
                  PixelAt(scaled, 19, 19), PixelAt(scaled, 20, 20),
                  PixelAt(scaled, 59, 59), PixelAt(scaled, 60, 60));
    }
  }

  // 7. Solid colour analysis, in both directions. A three-rectangle recording
  //    must refuse; a one-colour one must not, and must name the colour -
  //    because a tile cc knows is one colour is a tile it never allocates
  //    memory for.
  {
    SkColor4f colour;
    Check("a three-rectangle recording is not solid",
          !raster->PerformSolidColorAnalysis(gfx::Rect(0, 0, 64, 64), &colour,
                                             8));

    Painter solid_painter(layer_size);
    solid_painter.set_solid(true);
    cc::RecordingSource solid_recording;
    cc::Region invalidation{gfx::Rect(layer_size)};
    solid_recording.Update(layer_size, 1.0f, solid_painter, invalidation);
    scoped_refptr<cc::RasterSource> solid = solid_recording.CreateRasterSource();
    SkColor4f named;
    bool is_solid =
        solid->PerformSolidColorAnalysis(gfx::Rect(0, 0, 64, 64), &named, 8);
    Check("and a one-colour recording is solid, and names its colour",
          is_solid && named.toSkColor() == kBackground);
  }

  // 8. The tiling arithmetic itself. 64x64 of content in 32x32 tiles is four
  //    tiles; a point in the far quadrant is in tile (1,1); and that tile's
  //    bounds are the far quadrant. Every one of these is a number this
  //    program works out rather than reads back.
  {
    cc::TilingData tiling(gfx::Size(32, 32), gfx::Rect(0, 0, 64, 64),
                          /*border_texels=*/0);
    Check("cc::TilingData puts 64x64 of content in 32x32 tiles into four "
          "tiles",
          tiling.num_tiles_x() == 2 && tiling.num_tiles_y() == 2);
    Check("and a point in the far quadrant is in tile (1,1)",
          tiling.TileXIndexFromSrcCoord(40) == 1 &&
              tiling.TileYIndexFromSrcCoord(40) == 1 &&
              tiling.TileXIndexFromSrcCoord(31) == 0);
    Check("and that tile's bounds are the far quadrant exactly",
          tiling.TileBounds(1, 1) == gfx::Rect(32, 32, 32, 32));

    // A border texel is the pixel a neighbouring tile shares so that a
    // bilinear filter at the seam has something to read. It makes the tiles
    // overlap by one, which is visible in the bounds and nowhere else.
    cc::TilingData bordered(gfx::Size(32, 32), gfx::Rect(0, 0, 64, 64),
                            /*border_texels=*/1);
    Check("and with a border texel the tiles overlap by one, which is what "
          "keeps a bilinear filter from reading past a seam",
          bordered.TileBoundsWithBorder(0, 0).right() >
              bordered.TileBounds(0, 0).right());
  }

  if (failures != 0) {
    std::printf("chromiumcc2: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m160] cc's layer path on this machine: %d checks. A display list "
      "recorded over the interface Blink implements, an invalidation that "
      "refuses to repaint what is not dirty, a raster source replayed into "
      "tiles at two scales, and the tiling arithmetic underneath it - all on "
      "the software path, with no GL, no Vulkan and no GPU stack in the "
      "build.\n",
      checks);
  std::printf("chromiumcc2: done\n");
  return 0;
}
