#include "lean_os/ozone/leanos_surface_factory.h"

#include <string.h>

#include <algorithm>
#include <memory>

#include "base/logging.h"
#include "lean_os/ozone/leanos_window_manager.h"
#include "skia/ext/legacy_display_globals.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkSurfaceProps.h"
#include "ui/gfx/buffer_types.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_pixmap.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

class LeanOsCanvas : public SurfaceOzoneCanvas {
 public:
  explicit LeanOsCanvas(scoped_refptr<LeanOsWindowBuffer> buffer)
      : buffer_(std::move(buffer)) {}
  ~LeanOsCanvas() override = default;

  // SurfaceOzoneCanvas:
  void ResizeCanvas(const gfx::Size& viewport_size, float scale) override {
    SkSurfaceProps props = skia::LegacyDisplayGlobals::GetSkSurfaceProps();
    SkImageInfo info = SkImageInfo::MakeN32Premul(viewport_size.width(),
                                                  viewport_size.height());
    wrapped_ = false;
    if (buffer_ && buffer_->alive() && viewport_size == buffer_->size()) {
      surface_ = SkSurfaces::WrapPixels(info, buffer_->pixels(),
                                        info.minRowBytes(), &props);
      wrapped_ = surface_ != nullptr;
    }
    if (!wrapped_) {
      // viz asked for a size the compositor did not give this window, or
      // there is no window. Paint somewhere, and copy the part that fits.
      surface_ = SkSurfaces::Raster(info, &props);
    }
  }

  SkCanvas* GetCanvas() override { return surface_->getCanvas(); }

  void PresentCanvas(const gfx::Rect& damage) override {
    if (!buffer_ || !buffer_->alive())
      return;
    if (!wrapped_) {
      SkImageInfo info = surface_->getCanvas()->imageInfo();
      const int width = std::min(info.width(), buffer_->size().width());
      const int height = std::min(info.height(), buffer_->size().height());
      SkImageInfo row = SkImageInfo::MakeN32Premul(width, 1);
      for (int y = 0; y < height; y++) {
        surface_->getCanvas()->readPixels(
            row, buffer_->pixels() + static_cast<size_t>(y) * buffer_->size().width(),
            row.minRowBytes(), 0, y);
      }
    }
    buffer_->Present();
  }

  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return nullptr;
  }

 private:
  scoped_refptr<LeanOsWindowBuffer> buffer_;
  sk_sp<SkSurface> surface_;
  bool wrapped_ = false;
};

// The GPU-side objects ozone is obliged to hand out even on a platform with
// nothing to accelerate - the same stub headless uses, which is what //gpu
// asks for before //gpu/config has answered that there is no GPU here.
class LeanOsPixmap : public gfx::NativePixmap {
 public:
  explicit LeanOsPixmap(viz::SharedImageFormat format) : format_(format) {}

  LeanOsPixmap(const LeanOsPixmap&) = delete;
  LeanOsPixmap& operator=(const LeanOsPixmap&) = delete;

  bool AreDmaBufFdsValid() const override { return false; }
  int GetDmaBufFd(size_t plane) const override { return -1; }
  uint32_t GetDmaBufPitch(size_t plane) const override { return 0; }
  size_t GetDmaBufOffset(size_t plane) const override { return 0; }
  size_t GetDmaBufPlaneSize(size_t plane) const override { return 0; }
  uint64_t GetFormatModifier() const override { return 0; }
  viz::SharedImageFormat GetSharedImageFormat() const override { return format_; }
  size_t GetNumberOfPlanes() const override { return format_.NumberOfPlanes(); }
  bool SupportsZeroCopyWebGPUImport() const override { return false; }
  gfx::Size GetBufferSize() const override { return gfx::Size(); }
  uint32_t GetUniqueId() const override { return 0; }
  bool ScheduleOverlayPlane(gfx::AcceleratedWidget widget,
                            const gfx::OverlayPlaneData& overlay_plane_data,
                            std::vector<gfx::GpuFence> acquire_fences,
                            std::vector<gfx::GpuFence> release_fences) override {
    return true;
  }
  gfx::NativePixmapHandle ExportHandle() const override {
    return gfx::NativePixmapHandle();
  }

 private:
  ~LeanOsPixmap() override = default;

  viz::SharedImageFormat format_;
};

}  // namespace

LeanOsSurfaceFactory::LeanOsSurfaceFactory(LeanOsWindowManager* window_manager)
    : window_manager_(window_manager) {}

LeanOsSurfaceFactory::~LeanOsSurfaceFactory() = default;

std::vector<gl::GLImplementationParts>
LeanOsSurfaceFactory::GetAllowedGLImplementations() {
  // There is no GL on this machine (M158, M159). Naming ANGLE here, as
  // headless does, is what lets //gpu's startup ask and be told no by the
  // GLOzone that is not there, rather than by a list it cannot parse.
  return std::vector<gl::GLImplementationParts>{
      gl::GLImplementationParts(gl::kGLImplementationEGLANGLE),
  };
}

GLOzone* LeanOsSurfaceFactory::GetGLOzone(
    const gl::GLImplementationParts& implementation) {
  return nullptr;
}

std::unique_ptr<SurfaceOzoneCanvas> LeanOsSurfaceFactory::CreateCanvasForWidget(
    gfx::AcceleratedWidget widget) {
  return std::make_unique<LeanOsCanvas>(window_manager_->GetBuffer(widget));
}

scoped_refptr<gfx::NativePixmap> LeanOsSurfaceFactory::CreateNativePixmap(
    gfx::AcceleratedWidget widget,
    gpu::VulkanDeviceQueue* device_queue,
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::BufferUsage usage,
    std::optional<gfx::Size> framebuffer_size) {
  return new LeanOsPixmap(format);
}

}  // namespace ui
