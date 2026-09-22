// Where the pixels go: straight into the compositor's segment.
//
// viz's software output device asks ozone for a SurfaceOzoneCanvas per
// widget and paints each frame into the SkCanvas it returns. The canvas here
// is an SkSurface wrapped around the window's shared memory - no copy,
// because Skia's N32 premultiplied layout on x86-64 is B,G,R,A per pixel and
// the compositor's word is X,R,G,B read as a little-endian uint32_t, which
// are the same four bytes. NetSurf's port found the same coincidence with
// libnsfb's XRGB8888 (M113), and Skia's own test program measured it
// (M157). PresentCanvas is then a PRESENT action to the compositor.

#ifndef LEAN_OS_OZONE_LEANOS_SURFACE_FACTORY_H_
#define LEAN_OS_OZONE_LEANOS_SURFACE_FACTORY_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "ui/ozone/public/gl_ozone.h"
#include "ui/ozone/public/surface_factory_ozone.h"

namespace ui {

class LeanOsWindowManager;

class LeanOsSurfaceFactory : public SurfaceFactoryOzone {
 public:
  explicit LeanOsSurfaceFactory(LeanOsWindowManager* window_manager);

  LeanOsSurfaceFactory(const LeanOsSurfaceFactory&) = delete;
  LeanOsSurfaceFactory& operator=(const LeanOsSurfaceFactory&) = delete;

  ~LeanOsSurfaceFactory() override;

  // SurfaceFactoryOzone:
  std::vector<gl::GLImplementationParts> GetAllowedGLImplementations() override;
  GLOzone* GetGLOzone(const gl::GLImplementationParts& implementation) override;
  std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(
      gfx::AcceleratedWidget widget) override;
  scoped_refptr<gfx::NativePixmap> CreateNativePixmap(
      gfx::AcceleratedWidget widget,
      gpu::VulkanDeviceQueue* device_queue,
      gfx::Size size,
      viz::SharedImageFormat format,
      gfx::BufferUsage usage,
      std::optional<gfx::Size> framebuffer_size) override;

 private:
  raw_ptr<LeanOsWindowManager> window_manager_;
};

}  // namespace ui

#endif  // LEAN_OS_OZONE_LEANOS_SURFACE_FACTORY_H_
