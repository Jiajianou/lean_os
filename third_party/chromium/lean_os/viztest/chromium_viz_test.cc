// The display compositor, on this machine: independent surfaces aggregated
// into one frame, and that frame rastered on the CPU.
//
// M160 got //cc, which is the compositor a single renderer runs to turn its
// own layers into tiles. //components/viz/service is the other one - the
// compositor the BROWSER runs, whose input is not layers but whole
// CompositorFrames submitted by other processes, and whose output is the
// framebuffer. Everything a multi-process browser puts on a screen goes
// through it: the renderer submits a frame for its part of the window, the
// browser submits one for the chrome around it, and viz is what makes those
// two one picture.
//
// M161 measured this target and set it aside, correctly for what it knew:
// //components/viz/service IS the GPU service - //gpu/ipc/service and
// //gpu/command_buffer/service - so it wants the GL implementation compiled,
// which contradicts M158 and M159. What that measurement did not say, and
// what re-measuring it under M161's own flags does, is that BLINK ALREADY
// REACHES ALL OF IT: viz/service's graph is 6,555 targets and 6,533 of them
// are in blink's 8,504. The rung is 22 targets wide.
//
// So what this milestone costs is not the compositor. It is the nine targets
// in those 22 that are //sandbox - which is the second of the two conditions
// CLAUDE.md names, arriving five rungs before it was expected.
//
// WHAT IS GRADED, AND WHY IT IS PIXELS
//
// The same rule as M157, M158 and M160: a compositor that links and produces
// a blank bitmap passes every test that only asks whether the calls returned.
// Every expected value below is arithmetic this program does for itself.
//
// Two of the checks are not pixels and they are the sharper ones. A second
// DrawAndSwap with nothing submitted must NOT draw - a display compositor
// that redrew the screen whenever it was asked would produce identical
// pixels here and a laptop with no battery life - and the aggregated damage
// after a child changes must be the child's rectangle rather than the
// window. That is M160's pair of invariants one layer up, where they are
// about whole surfaces rather than about one layer's recording.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/time/time.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/quads/shared_quad_state.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/quads/surface_draw_quad.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/parent_local_surface_id_allocator.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "components/viz/common/surfaces/surface_range.h"
#include "components/viz/service/display/display.h"
#include "components/viz/service/display/display_client.h"
#include "components/viz/service/display/overlay_processor_stub.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_support.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/sync_point_manager.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/mask_filter_info.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumviz: %s\n", what);
  } else {
    std::printf("chromiumviz: FAIL %s\n", what);
    ++failures;
  }
}

// The framebuffer this display compositor draws into. 200x150 rather than a
// round number so that an off-by-one in the aggregator's transform arithmetic
// cannot land on a coincidence.
constexpr int kWidth = 200;
constexpr int kHeight = 150;

constexpr SkColor4f kRootBackground{0.0f, 0.0f, 0.0f, 1.0f};
constexpr SkColor4f kRootQuad{1.0f, 0.0f, 0.0f, 1.0f};
constexpr SkColor4f kChildQuad{0.0f, 1.0f, 0.0f, 1.0f};
constexpr SkColor4f kOverQuad{0.0f, 0.0f, 1.0f, 1.0f};

// The two frame sinks. In a browser one of these is the browser's own and the
// other belongs to a renderer in a different process; here they are two ids
// in one process, which is the same thing as far as viz is concerned - the
// aggregator knows a surface by its id and not by who filled it.
constexpr viz::FrameSinkId kRootSink(1, 1);
constexpr viz::FrameSinkId kChildSink(2, 1);

// Display::DrawAndSwap returns true whenever it got as far as aggregating,
// including the case where it then decided to skip the swap - so the return
// value is NOT the answer to "did this frame cost anything". The answer is
// the first argument of DisplayWillDrawAndSwap, which is Display's own
// should_draw, and the aggregated damage is the last render pass's
// damage_rect in the list handed to the same call. Recording them here is
// how this program grades the two invariants that are not pixels.
class RecordingDisplayClient : public viz::DisplayClient {
 public:
  void DisplayOutputSurfaceLost() override {}
  void DisplayWillDrawAndSwap(bool will_draw_and_swap,
                              viz::AggregatedRenderPassList* render_passes) override {
    will_draw_ = will_draw_and_swap;
    damage_ = render_passes && !render_passes->empty()
                  ? render_passes->back()->damage_rect
                  : gfx::Rect();
  }
  void DisplayDidDrawAndSwap() override {}
  void DisplayDidReceiveCALayerParams(gfx::CALayerParams ca_layer_params) override {}
  void DisplayDidCompleteSwapWithSize(const gfx::Size& pixel_size) override {}
  void DisplayAddChildWindowToBrowser(gpu::SurfaceHandle child_window) override {}
  void SetWideColorEnabled(bool enabled) override {}

  bool will_draw() const { return will_draw_; }
  const gfx::Rect& damage() const { return damage_; }

 private:
  bool will_draw_ = false;
  gfx::Rect damage_;
};

uint32_t PixelAt(const SkBitmap& bitmap, int x, int y) {
  if (x < 0 || y < 0 || x >= bitmap.width() || y >= bitmap.height()) {
    return 0;
  }
  return bitmap.getColor(x, y);
}

bool IsColor(const SkBitmap& bitmap, int x, int y, SkColor expected) {
  return PixelAt(bitmap, x, y) == expected;
}

// What the framebuffer actually holds at the points the checks look at.
// Printed unconditionally rather than only on failure: a colour that comes
// back through a colour space conversion is a fact about this build worth
// having in the log even when every check passes.
void ReportPixels(const char* when, const SkBitmap& bitmap) {
  static const struct {
    const char* what;
    int x;
    int y;
  } points[] = {
      {"background", 5, 140},   {"root quad", 130, 40},
      {"child green", 55, 55},  {"child inner", 35, 30},
      {"outside child", 95, 55}, {"blend", 10, 10},
  };
  for (const auto& p : points) {
    std::printf("chromiumviz:   %s %s (%d,%d) = %08x\n", when, p.what, p.x,
                p.y, PixelAt(bitmap, p.x, p.y));
  }
}

// A quad list is sorted FRONT TO BACK - quad_list.h says so and
// DirectRenderer draws it with BackToFrontBegin() - which is the opposite of
// the order a person writes a painter's algorithm in. Appending the
// background first, the way one would paint it, puts an opaque quad in front
// of everything else: the frame still aggregates, the damage is still right
// and every structural check still passes, and the screen is black.
//
// A CompositorFrame with one render pass and one solid colour quad in it,
// which is the smallest thing a client can submit that puts something on a
// screen.
// Every client keeps one of these and stamps each frame with ++it: a frame
// token is how the presentation feedback for a frame finds its way back to
// the client that submitted it, and 0 is the invalid value. Leaving it at
// the default reaches a CHECK inside CompositorFrameSinkSupport rather than
// a wrong picture, which is the display compositor being right about a
// client that is wrong.
viz::CompositorFrame MakeFrame(viz::FrameTokenGenerator* tokens,
                               const gfx::Rect& output_rect,
                               const gfx::Rect& damage_rect) {
  viz::CompositorFrame frame;
  frame.metadata.device_scale_factor = 1.0f;
  frame.metadata.frame_token = ++(*tokens);
  frame.metadata.begin_frame_ack =
      viz::BeginFrameAck::CreateManualAckWithDamage();
  auto pass = viz::CompositorRenderPass::Create();
  pass->SetNew(viz::CompositorRenderPassId{1}, output_rect, damage_rect,
               gfx::Transform());
  frame.render_pass_list.push_back(std::move(pass));
  return frame;
}

// Where a quad goes is the SharedQuadState's transform, not the quad's own
// origin - which matters most for the SurfaceDrawQuad below, whose rect is
// the embedded surface's bounds in the CHILD's coordinate space. Getting
// that backwards draws a child surface at the top left of the window and
// leaves every other check here passing.
viz::SharedQuadState* AppendSharedQuadState(viz::CompositorRenderPass* pass,
                                            const gfx::Transform& transform,
                                            const gfx::Rect& rect,
                                            float opacity) {
  viz::SharedQuadState* state = pass->CreateAndAppendSharedQuadState();
  state->SetAll(transform, rect, rect, gfx::MaskFilterInfo(),
                /*clip=*/std::nullopt, /*contents_opaque=*/opacity == 1.0f,
                opacity, SkBlendMode::kSrcOver, /*sorting_context=*/0,
                /*layer_id=*/0u, /*fast_rounded_corner=*/false);
  return state;
}

void AppendSolidColor(viz::CompositorRenderPass* pass,
                      const gfx::Rect& rect,
                      SkColor4f color,
                      float opacity) {
  viz::SharedQuadState* state =
      AppendSharedQuadState(pass, gfx::Transform(), rect, opacity);
  auto* quad = pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
  quad->SetNew(state, rect, rect, color, /*anti_aliasing_off=*/true);
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::SingleThreadTaskExecutor task_executor;

  std::printf("chromiumviz: starting\n");

  // The display compositor's own dependencies. On a machine with a GPU these
  // two are the GPU service's; DisplayResourceProviderSoftware takes them
  // anyway, because a software display still names its resources the same way.
  gpu::SharedImageManager shared_image_manager;
  gpu::SyncPointManager sync_point_manager;
  gpu::Scheduler gpu_scheduler(&sync_point_manager);

  viz::FrameSinkManagerImpl::InitParams init_params;
  viz::FrameSinkManagerImpl frame_sink_manager(init_params);

  // The base SoftwareOutputDevice is the framebuffer: it allocates a raster
  // SkSurface in Resize() and hands DirectRenderer its canvas. Keeping the
  // pointer is how this program reads back what viz drew.
  auto output_device = std::make_unique<viz::SoftwareOutputDevice>();
  viz::SoftwareOutputDevice* output_device_raw = output_device.get();
  auto output_surface =
      std::make_unique<viz::SoftwareOutputSurface>(std::move(output_device));

  viz::RendererSettings renderer_settings;
  viz::DebugRendererSettings debug_settings;

  // The scheduler is null on purpose. Display's own comment says that when it
  // is, DrawAndSwap must be called externally - which is what makes a frame
  // something this program decides to produce rather than something a vsync
  // timer produces while the checks run.
  auto display = std::make_unique<viz::Display>(
      &shared_image_manager, &gpu_scheduler, renderer_settings, &debug_settings,
      kRootSink, /*gpu_dependency=*/nullptr, std::move(output_surface),
      std::make_unique<viz::OverlayProcessorStub>(), /*scheduler=*/nullptr,
      task_executor.task_runner());

  RecordingDisplayClient display_client;
  frame_sink_manager.RegisterFrameSinkId(kRootSink, /*report_activation=*/true);
  frame_sink_manager.RegisterFrameSinkId(kChildSink, /*report_activation=*/true);

  auto root_support = std::make_unique<viz::CompositorFrameSinkSupport>(
      nullptr, &frame_sink_manager, kRootSink, /*is_root=*/true);
  auto child_support = std::make_unique<viz::CompositorFrameSinkSupport>(
      nullptr, &frame_sink_manager, kChildSink, /*is_root=*/false);

  display->Initialize(&display_client, frame_sink_manager.surface_manager());
  display->SetVisible(true);
  display->Resize(gfx::Size(kWidth, kHeight));

  Check("a software display compositor is constructed and initialized", true);

  viz::FrameTokenGenerator root_tokens;
  viz::FrameTokenGenerator child_tokens;
  viz::ParentLocalSurfaceIdAllocator root_allocator;
  viz::ParentLocalSurfaceIdAllocator child_allocator;
  root_allocator.GenerateId();
  child_allocator.GenerateId();
  const viz::LocalSurfaceId root_id = root_allocator.GetCurrentLocalSurfaceId();
  const viz::LocalSurfaceId child_id = child_allocator.GetCurrentLocalSurfaceId();
  const viz::SurfaceId child_surface_id(kChildSink, child_id);

  display->SetLocalSurfaceId(root_id, 1.0f);

  // Frame one: the child submits its own surface, 60x40, filled green with a
  // 20x20 blue square in the top left of it. The root embeds the child at
  // (30,25) and paints a red rectangle of its own at (120,20).
  const gfx::Rect child_rect(0, 0, 60, 40);
  const gfx::Rect child_inner(0, 0, 20, 20);
  const gfx::Rect child_at(30, 25, 60, 40);
  const gfx::Rect root_rect(120, 20, 50, 60);

  {
    viz::CompositorFrame frame = MakeFrame(&child_tokens, child_rect, child_rect);
    viz::CompositorRenderPass* pass = frame.render_pass_list.back().get();
    AppendSolidColor(pass, child_inner, kOverQuad, 1.0f);
    AppendSolidColor(pass, child_rect, kChildQuad, 1.0f);
    child_support->SubmitCompositorFrame(child_id, std::move(frame));
  }

  {
    viz::CompositorFrame frame =
        MakeFrame(&root_tokens, gfx::Rect(0, 0, kWidth, kHeight),
                  gfx::Rect(0, 0, kWidth, kHeight));
    viz::CompositorRenderPass* pass = frame.render_pass_list.back().get();
    viz::SharedQuadState* state = AppendSharedQuadState(
        pass, gfx::Transform::MakeTranslation(child_at.x(), child_at.y()),
        child_rect, 1.0f);
    auto* quad = pass->CreateAndAppendDrawQuad<viz::SurfaceDrawQuad>();
    quad->SetNew(state, child_rect, child_rect,
                 viz::SurfaceRange(child_surface_id), SkColors::kWhite,
                 /*stretch_content=*/false);
    AppendSolidColor(pass, root_rect, kRootQuad, 1.0f);
    AppendSolidColor(pass, gfx::Rect(0, 0, kWidth, kHeight), kRootBackground,
                     1.0f);
    frame.metadata.referenced_surfaces.push_back(
        viz::SurfaceRange(child_surface_id));
    root_support->SubmitCompositorFrame(root_id, std::move(frame));
  }

  viz::DrawAndSwapParams draw_params;
  draw_params.expected_display_time = base::TimeTicks::Now();
  display->DrawAndSwap(draw_params);
  Check("a submitted frame draws", display_client.will_draw());

  base::RunLoop().RunUntilIdle();

  SkBitmap bitmap = output_device_raw->ReadbackForTesting();
  Check("the framebuffer is the size the display was resized to",
        bitmap.width() == kWidth && bitmap.height() == kHeight);

  ReportPixels("frame one", bitmap);
  std::printf("chromiumviz:   expected background=%08x root=%08x child=%08x inner=%08x\n",
              kRootBackground.toSkColor(), kRootQuad.toSkColor(),
              kChildQuad.toSkColor(), kOverQuad.toSkColor());

  const SkColor black = kRootBackground.toSkColor();
  const SkColor red = kRootQuad.toSkColor();
  const SkColor green = kChildQuad.toSkColor();
  const SkColor blue = kOverQuad.toSkColor();

  Check("the root's background covers a pixel no quad claims",
        IsColor(bitmap, 5, 140, black));

  // The root's own quad, at its exact edges. 120..169 inclusive: 170 is
  // outside it and must still be the background.
  Check("the root's own quad lands on its exact rectangle",
        IsColor(bitmap, 120, 20, red) && IsColor(bitmap, 169, 79, red) &&
            IsColor(bitmap, 119, 20, black) && IsColor(bitmap, 170, 79, black) &&
            IsColor(bitmap, 120, 19, black) && IsColor(bitmap, 169, 80, black));

  // The aggregator's whole job in one check: the child submitted a frame in
  // ITS OWN coordinate space, where the green starts at (0,0), and it has to
  // appear at the root's (30,25) because that is where the SurfaceDrawQuad
  // put it. A viz that ignored the transform would draw the child at 0,0 and
  // every other check here would still pass.
  Check("a child surface is aggregated at the root's coordinates",
        IsColor(bitmap, 55, 55, green) && IsColor(bitmap, 29, 25, black) &&
            IsColor(bitmap, 30, 24, black));

  Check("the child surface's far edge is where the transform puts it",
        IsColor(bitmap, 89, 64, green) && IsColor(bitmap, 90, 64, black) &&
            IsColor(bitmap, 89, 65, black));

  // The blue square is inside the child's own frame, drawn after the green,
  // so it wins there - and it is offset by the same aggregation transform.
  Check("quad order inside the child survives aggregation",
        IsColor(bitmap, 30, 25, blue) && IsColor(bitmap, 49, 44, blue) &&
            IsColor(bitmap, 50, 44, green) && IsColor(bitmap, 49, 45, green));

  // Nothing has been submitted since the draw. A display compositor that
  // drew anyway would produce a correct picture and an unusable machine.
  display->DrawAndSwap(draw_params);
  Check("a draw with nothing newly submitted does not draw",
        !display_client.will_draw() && display_client.damage().IsEmpty());

  // Frame two: only the child changes, and only part of it. What the
  // aggregator reports as damage has to be the part that changed rather than
  // the window - this is the number a browser uses to decide how much of the
  // screen to push to the hardware.
  const gfx::Rect child_damage(0, 0, 20, 20);
  {
    viz::CompositorFrame frame = MakeFrame(&child_tokens, child_rect, child_damage);
    viz::CompositorRenderPass* pass = frame.render_pass_list.back().get();
    AppendSolidColor(pass, child_inner, kRootQuad, 1.0f);
    AppendSolidColor(pass, child_rect, kChildQuad, 1.0f);
    child_support->SubmitCompositorFrame(child_id, std::move(frame));
  }

  display->DrawAndSwap(draw_params);
  Check("a new frame from a child draws", display_client.will_draw());

  // And this is the number a browser pushes to the hardware. The child
  // damaged a 20x20 rectangle in ITS own coordinates; what comes out of the
  // aggregator has to be that rectangle at the root's (30,25), because a
  // compositor that reported the window here would repaint a screen to move
  // a caret.
  Check("the aggregated damage is the child's rectangle, not the window",
        display_client.damage() == gfx::Rect(30, 25, 20, 20));

  base::RunLoop().RunUntilIdle();
  bitmap = output_device_raw->ReadbackForTesting();

  Check("the child's changed pixels are the changed ones",
        IsColor(bitmap, 30, 25, red) && IsColor(bitmap, 49, 44, red) &&
            IsColor(bitmap, 50, 44, green));

  Check("the root's own quad is untouched by the child's frame",
        IsColor(bitmap, 120, 20, red) && IsColor(bitmap, 169, 79, red));

  // Half opacity over black is the midpoint, on the CPU, in this renderer.
  // 0.5 of 255 through a premultiplied sRGB blend lands on 0x80 the same way
  // M157's Skia blend did - and this is the path that gets there through a
  // SharedQuadState rather than through an SkPaint.
  {
    viz::CompositorFrame frame =
        MakeFrame(&root_tokens, gfx::Rect(0, 0, kWidth, kHeight),
                  gfx::Rect(0, 0, kWidth, kHeight));
    viz::CompositorRenderPass* pass = frame.render_pass_list.back().get();
    AppendSolidColor(pass, gfx::Rect(0, 0, 40, 40), SkColors::kWhite, 0.5f);
    AppendSolidColor(pass, gfx::Rect(0, 0, kWidth, kHeight), kRootBackground,
                     1.0f);
    root_allocator.GenerateId();
    const viz::LocalSurfaceId next_root =
        root_allocator.GetCurrentLocalSurfaceId();
    display->SetLocalSurfaceId(next_root, 1.0f);
    root_support->SubmitCompositorFrame(next_root, std::move(frame));
  }

  display->DrawAndSwap(draw_params);
  Check("a frame with an opacity draws", display_client.will_draw());
  base::RunLoop().RunUntilIdle();
  bitmap = output_device_raw->ReadbackForTesting();

  const uint32_t blended = PixelAt(bitmap, 10, 10);
  Check("half opacity over black is the midpoint",
        SkColorGetR(blended) == 0x80 && SkColorGetG(blended) == 0x80 &&
            SkColorGetB(blended) == 0x80);

  Check("the child surface is gone when the root stops embedding it",
        IsColor(bitmap, 55, 55, black));

  if (failures != 0) {
    std::printf("chromiumviz: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m162] the display compositor on this machine: %d checks. Two surfaces "
      "from two frame sinks aggregated into one frame and rastered on the "
      "CPU - the child at the root's coordinates, quad order and opacity "
      "through the blend, a draw that refuses when nothing was submitted, and "
      "damage that is the child's rectangle rather than the window.\n",
      checks);
  std::printf("chromiumviz: done\n");
  return 0;
}
